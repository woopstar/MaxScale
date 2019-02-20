/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2022-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

/**
 * @file monitor.c  - The monitor module management routines
 */
#include <maxscale/monitor.hh>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <string>
#include <sstream>
#include <set>
#include <zlib.h>
#include <sys/stat.h>
#include <vector>
#include <mutex>

#include <maxscale/alloc.h>
#include <maxbase/atomic.hh>
#include <maxscale/clock.h>
#include <maxscale/json_api.h>
#include <maxscale/mariadb.hh>
#include <maxscale/mysql_utils.hh>
#include <maxscale/paths.h>
#include <maxscale/pcre2.h>
#include <maxscale/routingworker.hh>
#include <maxscale/secrets.h>
#include <maxscale/utils.hh>
#include <maxscale/json_api.h>
#include <mysqld_error.h>

#include "internal/config.hh"
#include "internal/externcmd.hh"
#include "internal/monitor.hh"
#include "internal/modules.hh"
#include "internal/server.hh"
#include "internal/service.hh"

/** Schema version, journals must have a matching version */
#define MMB_SCHEMA_VERSION 2

/** Constants for byte lengths of the values */
#define MMB_LEN_BYTES          4
#define MMB_LEN_SCHEMA_VERSION 1
#define MMB_LEN_CRC32          4
#define MMB_LEN_VALUE_TYPE     1
#define MMB_LEN_SERVER_STATUS  8

/** Type of the stored value */
enum stored_value_type
{
    SVT_SERVER = 1,     // Generic server state information
    SVT_MASTER = 2,     // The master server name
};

using std::string;
using std::set;
using Guard = std::lock_guard<std::mutex>;

const char CN_BACKEND_CONNECT_ATTEMPTS[] = "backend_connect_attempts";
const char CN_BACKEND_CONNECT_TIMEOUT[] = "backend_connect_timeout";
const char CN_BACKEND_READ_TIMEOUT[] = "backend_read_timeout";
const char CN_BACKEND_WRITE_TIMEOUT[] = "backend_write_timeout";
const char CN_DISK_SPACE_CHECK_INTERVAL[] = "disk_space_check_interval";
const char CN_EVENTS[] = "events";
const char CN_JOURNAL_MAX_AGE[] = "journal_max_age";
const char CN_MONITOR_INTERVAL[] = "monitor_interval";
const char CN_SCRIPT[] = "script";
const char CN_SCRIPT_TIMEOUT[] = "script_timeout";

namespace
{

class ThisUnit
{
public:

    /**
     * Call a function on every monitor in the global monitor list.
     *
     * @param apply The function to apply. If the function returns false, iteration is discontinued.
     */
    void foreach_monitor(std::function<bool(Monitor*)> apply)
    {
        Guard guard(m_all_monitors_lock);
        for (Monitor* monitor : m_all_monitors)
        {
            if (!apply(monitor))
            {
                break;
            }
        }
    }

    /**
     * Clear the internal list and return previous contents.
     *
     * @return Contents before clearing
     */
    std::vector<Monitor*> clear()
    {
        Guard guard(m_all_monitors_lock);
        return std::move(m_all_monitors);
    }

    void insert_front(Monitor* monitor)
    {
        Guard guard(m_all_monitors_lock);
        m_all_monitors.insert(m_all_monitors.begin(), monitor);
    }

    void run_behind_lock(std::function<void(void)> apply)
    {
        Guard guard(m_all_monitors_lock);
        apply();
    }

private:
    std::mutex m_all_monitors_lock;        /**< Protects access to array */
    std::vector<Monitor*> m_all_monitors;  /**< Global list of monitors, in configuration file order */
};

ThisUnit this_unit;

}

static void        monitor_server_free_all(std::vector<MXS_MONITORED_SERVER*>& servers);
static void        remove_server_journal(Monitor* monitor);
static const char* monitor_state_to_string(monitor_state_t state);

/** Server type specific bits */
static uint64_t server_type_bits = SERVER_MASTER | SERVER_SLAVE | SERVER_JOINED | SERVER_NDB;

/** All server bits */
static uint64_t all_server_bits = SERVER_RUNNING | SERVER_MAINT | SERVER_MASTER | SERVER_SLAVE
    | SERVER_JOINED | SERVER_NDB;

Monitor* MonitorManager::create_monitor(const string& name, const string& module,
                                        MXS_CONFIG_PARAMETER* params)
{
    MXS_MONITOR_API* api = (MXS_MONITOR_API*)load_module(module.c_str(), MODULE_MONITOR);
    if (!api)
    {
        MXS_ERROR("Unable to load library file for monitor '%s'.", name.c_str());
        return NULL;
    }

    Monitor* mon = api->createInstance(name, module);
    if (!mon)
    {
        MXS_ERROR("Unable to create monitor instance for '%s', using module '%s'.",
                  name.c_str(), module.c_str());
        return NULL;
    }

    if (mon->configure_base(params)) // TODO: Move derived class configure() here
    {
        this_unit.insert_front(mon);
    }
    else
    {
        delete mon;
        mon = NULL;
    }
    return mon;
}

Monitor::Monitor(const string& name, const string& module)
    : m_name(MXS_STRDUP_A(name.c_str()))
    , m_module(module)
{
    memset(m_journal_hash, 0, sizeof(m_journal_hash));
}

void Monitor::stop()
{
    do_stop();

    for (auto db : m_servers)
    {
        // TODO: Should be db->close().
        mysql_close(db->con);
        db->con = NULL;
    }
}

bool Monitor::configure_base(const MXS_CONFIG_PARAMETER* params)
{
    m_settings.conn_settings.read_timeout = params->get_integer(CN_BACKEND_READ_TIMEOUT);
    m_settings.conn_settings.write_timeout = params->get_integer(CN_BACKEND_WRITE_TIMEOUT);
    m_settings.conn_settings.connect_timeout = params->get_integer(CN_BACKEND_CONNECT_TIMEOUT);
    m_settings.conn_settings.connect_attempts = params->get_integer(CN_BACKEND_CONNECT_ATTEMPTS);
    m_settings.interval = params->get_integer(CN_MONITOR_INTERVAL);
    m_settings.journal_max_age = params->get_integer(CN_JOURNAL_MAX_AGE);
    m_settings.script_timeout = params->get_integer(CN_SCRIPT_TIMEOUT);
    m_settings.script = params->get_string(CN_SCRIPT);
    m_settings.events = params->get_enum(CN_EVENTS, mxs_monitor_event_enum_values);
    m_settings.disk_space_check_interval = params->get_integer(CN_DISK_SPACE_CHECK_INTERVAL);
    m_settings.conn_settings.username = params->get_string(CN_USER);
    m_settings.conn_settings.password = params->get_string(CN_PASSWORD);

    // The monitor serverlist has already been checked to be valid. Empty value is ok too.
    auto servers_temp = params->get_server_list(CN_SERVERS);
    for (auto elem : servers_temp)
    {
        // This function checks if server is already monitored. TODO: This should be a config error.
        Monitor::add_server(this, elem);
    }

    /* The previous config values were normal types and were checked by the config manager
     * to be correct. The following is a complicated type and needs to be checked separately. */
    bool error = false;
    auto threshold_string = params->get_string(CN_DISK_SPACE_THRESHOLD);
    if (!set_disk_space_threshold(threshold_string))
    {
        MXS_ERROR("Invalid value for '%s' for monitor %s: %s",
                  CN_DISK_SPACE_THRESHOLD, m_name, threshold_string.c_str());
        error = true;
    }

    if (!error)
    {
        // Store module name into parameter storage.
        parameters.set(CN_MODULE, m_module);
        // Add all config settings to text-mode storage. Needed for serialization.
        parameters.set_multiple(*params);
    }
    return !error;
}

Monitor::~Monitor()
{
    monitor_server_free_all(m_servers);
    MXS_FREE((const_cast<char*>(m_name)));
}

void MonitorManager::destroy_all_monitors()
{
    auto monitors = this_unit.clear();
    for (auto monitor : monitors)
    {
        mxb_assert(monitor->state() == MONITOR_STATE_STOPPED);
        delete monitor;
    }
}

/**
 * Start an individual monitor that has previously been stopped.
 *
 * @param monitor The Monitor that should be started
 */
void MonitorManager::monitor_start(Monitor* monitor, const MXS_CONFIG_PARAMETER* params)
{
    mxb_assert(monitor);

    Guard guard(monitor->m_lock);

    // Only start the monitor if it's stopped.
    if (monitor->state() == MONITOR_STATE_STOPPED)
    {
        if (!monitor->start(params))
        {
            MXS_ERROR("Failed to start monitor '%s'.", monitor->m_name);
        }
    }
}

void MonitorManager::populate_services()
{
    this_unit.foreach_monitor([](Monitor* pMonitor) -> bool {
            pMonitor->populate_services();
            return true;
        });
}

/**
 * Start all monitors
 */
void monitor_start_all()
{
    this_unit.foreach_monitor([](Monitor* monitor) {
        if (monitor->m_active)
        {
            MonitorManager::monitor_start(monitor, &monitor->parameters);
        }
        return true;
    });
}

/**
 * Stop a given monitor
 *
 * @param monitor       The monitor to stop
 */
void monitor_stop(Monitor* monitor)
{
    mxb_assert(monitor);

    Guard guard(monitor->m_lock);

    /** Only stop the monitor if it is running */
    if (monitor->state() == MONITOR_STATE_RUNNING)
    {
        monitor->stop();
    }
}

void monitor_deactivate(Monitor* monitor)
{
    this_unit.run_behind_lock([monitor](){
        monitor->m_active = false;
    });
}

/**
 * Shutdown all running monitors
 */
void monitor_stop_all()
{
    this_unit.foreach_monitor([](Monitor* monitor) {
        if (monitor->m_active)
        {
            monitor_stop(monitor);
        }
        return true;
    });
}

//static
bool Monitor::add_server(Monitor* mon, SERVER* server)
{
    mxb_assert(mon && server);
    bool rval = false;

    if (monitor_server_in_use(server))
    {
        MXS_ERROR("Server '%s' is already monitored.", server->name());
    }
    else
    {
        rval = true;

        monitor_state_t old_state = mon->state();

        if (old_state == MONITOR_STATE_RUNNING)
        {
            monitor_stop(mon);
        }

        mon->add_server(server);

        if (old_state == MONITOR_STATE_RUNNING)
        {
            MonitorManager::monitor_start(mon, &mon->parameters);
        }
    }

    return rval;
}

/**
 * @brief Add a server to the monitor.
 *
 * It is assumed that the monitor is currently not running and that the
 * server is not currently being monitored.
 *
 * @param server  A server.
 */
void Monitor::add_server(SERVER* server)
{
    mxb_assert(state() != MONITOR_STATE_RUNNING);
    mxb_assert(!monitor_server_in_use(server));

    MXS_MONITORED_SERVER* db = new (std::nothrow) MXS_MONITORED_SERVER(server);
    MXS_ABORT_IF_NULL(db);

    using Guard = std::unique_lock<std::mutex>;
    Guard guard(m_lock);

    m_servers.push_back(db);

    guard.unlock();

    server_added(server);
}

void Monitor::server_added(SERVER* server)
{
    service_add_server(this, server);
}

void Monitor::server_removed(SERVER* server)
{
    service_remove_server(this, server);
}

static void monitor_server_free(MXS_MONITORED_SERVER* tofree)
{
    if (tofree)
    {
        if (tofree->con)
        {
            mysql_close(tofree->con);
        }
        delete tofree;
    }
}

/**
 * Free monitor server list
 * @param servers Servers to free
 */
static void monitor_server_free_all(std::vector<MXS_MONITORED_SERVER*>& servers)
{
    for (auto server : servers)
    {
        monitor_server_free(server);
    }
    servers.clear();
}

/**
 * Remove a server from a monitor.
 *
 * @param mon           The Monitor instance
 * @param server        The Server to remove
 */
//static
void Monitor::remove_server(Monitor* mon, SERVER* server)
{
    monitor_state_t old_state = mon->state();

    if (old_state == MONITOR_STATE_RUNNING)
    {
        monitor_stop(mon);
    }

    mon->remove_server(server);

    if (old_state == MONITOR_STATE_RUNNING)
    {
        MonitorManager::monitor_start(mon, &mon->parameters);
    }
}

/**
 * @brief Remove a server from the monitor.
 *
 * It is assumed that the monitor is currently not running.
 *
 * @param server  A server
 */
void Monitor::remove_server(SERVER* server)
{
    mxb_assert(state() != MONITOR_STATE_RUNNING);

    MXS_MONITORED_SERVER* ptr = nullptr;

    using Guard = std::unique_lock<std::mutex>;
    Guard guard(m_lock);

    for (auto it = m_servers.begin(); it != m_servers.end(); ++it)
    {
        if ((*it)->server == server)
        {
            ptr = *it;
            m_servers.erase(it);
            break;
        }
    }

    guard.unlock();

    if (ptr)
    {
        monitor_server_free(ptr);

        server_removed(server);
    }

}

void Monitor::set_user(const string& user)
{
    m_settings.conn_settings.username = user;
}

void Monitor::set_password(const string& passwd)
{
    m_settings.conn_settings.password = passwd;
}

/**
 * Show all monitors
 *
 * @param dcb   DCB for printing output
 */
void monitor_show_all(DCB* dcb)
{
    this_unit.foreach_monitor([dcb](Monitor* monitor) {
        if (monitor->m_active)
        {
            monitor_show(dcb, monitor);
        }
        return true;
    });
}

/**
 * Show a single monitor
 *
 * @param dcb   DCB for printing output
 */
void monitor_show(DCB* dcb, Monitor* monitor)
{
    monitor->show(dcb);
}

void Monitor::show(DCB* dcb)
{
    Monitor* monitor = this;
    dcb_printf(dcb, "Monitor:                %p\n", monitor);
    dcb_printf(dcb, "Name:                   %s\n", m_name);
    dcb_printf(dcb, "State:                  %s\n", monitor_state_to_string(state()));
    dcb_printf(dcb, "Times monitored:        %lu\n", m_ticks);
    dcb_printf(dcb, "Sampling interval:      %lu milliseconds\n", m_settings.interval);
    dcb_printf(dcb, "Connect Timeout:        %i seconds\n", m_settings.conn_settings.connect_timeout);
    dcb_printf(dcb, "Read Timeout:           %i seconds\n", m_settings.conn_settings.read_timeout);
    dcb_printf(dcb, "Write Timeout:          %i seconds\n", m_settings.conn_settings.write_timeout);
    dcb_printf(dcb, "Connect attempts:       %i \n", m_settings.conn_settings.connect_attempts);
    dcb_printf(dcb, "Monitored servers:      ");

    const char* sep = "";

    for (MXS_MONITORED_SERVER* db : monitor->m_servers)
    {
        dcb_printf(dcb, "%s[%s]:%d", sep, db->server->address, db->server->port);
        sep = ", ";
    }

    dcb_printf(dcb, "\n");

    if (state() == MONITOR_STATE_RUNNING)
    {
        monitor->diagnostics(dcb);
    }
    else
    {
        dcb_printf(dcb, " (no diagnostics)\n");
    }
    dcb_printf(dcb, "\n");
}

/**
 * List all the monitors
 *
 * @param dcb   DCB for printing output
 */
void monitor_list(DCB* dcb)
{
    dcb_printf(dcb, "---------------------+---------------------\n");
    dcb_printf(dcb, "%-20s | Status\n", "Monitor");
    dcb_printf(dcb, "---------------------+---------------------\n");

    this_unit.foreach_monitor([dcb](Monitor* ptr){
        if (ptr->m_active)
        {
            dcb_printf(dcb,
                       "%-20s | %s\n",
                       ptr->m_name,
                       ptr->state() == MONITOR_STATE_RUNNING ?
                       "Running" : "Stopped");
        }
        return true;
    });

    dcb_printf(dcb, "---------------------+---------------------\n");
}

/**
 * Find a monitor by name
 *
 * @param       name    The name of the monitor
 * @return      Pointer to the monitor or NULL
 */
Monitor* monitor_find(const char* name)
{
    Monitor* rval = nullptr;
    this_unit.foreach_monitor([&rval, name](Monitor* ptr) {
        if (!strcmp(ptr->m_name, name) && ptr->m_active)
        {
            rval = ptr;
        }
        return (rval == nullptr);
    });
    return rval;
}
/**
 * Find a destroyed monitor by name
 *
 * @param name The name of the monitor
 * @return  Pointer to the destroyed monitor or NULL if monitor is not found
 */
Monitor* monitor_repurpose_destroyed(const char* name, const char* module)
{
    Monitor* rval = NULL;
    this_unit.foreach_monitor([&rval, name, module](Monitor* monitor) {
        if (strcmp(monitor->m_name, name) == 0 && (monitor->m_module == module))
        {
            mxb_assert(!monitor->m_active);
            monitor->m_active = true;
            rval = monitor;
        }
        return (rval == nullptr);
    });
    return rval;
}

/**
 * Set the monitor sampling interval.
 *
 * @param interval The sampling interval in milliseconds
 */
void Monitor::set_interval(int64_t interval)
{
    m_settings.interval = interval;
}

/**
 * Set the maximum age of the monitor journal
 *
 * @param interval      The journal age in seconds
 */
void Monitor::monitor_set_journal_max_age(time_t value)
{
    m_settings.journal_max_age = value;
}

void Monitor::set_script_timeout(int value)
{
    m_settings.script_timeout = value;
}

bool Monitor::set_network_timeout(int type, int value, const char* key)
{
    bool rval = true;

    if (value > 0)
    {
        switch (type)
        {
        case MONITOR_CONNECT_TIMEOUT:
            m_settings.conn_settings.connect_timeout = value;
            break;

        case MONITOR_READ_TIMEOUT:
            m_settings.conn_settings.read_timeout = value;
            break;

        case MONITOR_WRITE_TIMEOUT:
            m_settings.conn_settings.write_timeout = value;
            break;

        case MONITOR_CONNECT_ATTEMPTS:
            m_settings.conn_settings.connect_attempts = value;
            break;

        default:
            MXS_ERROR("Monitor setNetworkTimeout received an unsupported action type %i", type);
            mxb_assert(!true);
            rval = false;
            break;
        }
    }
    else
    {
        MXS_ERROR("Value '%s' for monitor '%s' is not a positive integer: %d", key, m_name, value);
        rval = false;
    }
    return rval;
}

/**
 * Return a resultset that has the current set of monitors in it
 *
 * @return A Result set
 */
std::unique_ptr<ResultSet> monitor_get_list()
{
    std::unique_ptr<ResultSet> set = ResultSet::create({"Monitor", "Status"});
    this_unit.foreach_monitor([&set](Monitor* ptr) {
        const char* state = ptr->state() == MONITOR_STATE_RUNNING ? "Running" : "Stopped";
        set->add_row({ptr->m_name, state});
        return true;
    });
    return set;
}

bool Monitor::test_permissions(const string& query)
{
    auto monitor = this;
    if (monitor->m_servers.empty() || config_get_global_options()->skip_permission_checks)
    {
        return true;
    }

    char* dpasswd = decrypt_password(m_settings.conn_settings.password.c_str());
    bool rval = false;

    for (MXS_MONITORED_SERVER* mondb : monitor->m_servers)
    {
        if (!mon_connection_is_ok(mondb->ping_or_connect(m_settings.conn_settings)))
        {
            MXS_ERROR("[%s] Failed to connect to server '%s' ([%s]:%d) when"
                      " checking monitor user credentials and permissions: %s",
                      monitor->m_name,
                      mondb->server->name(),
                      mondb->server->address,
                      mondb->server->port,
                      mysql_error(mondb->con));
            switch (mysql_errno(mondb->con))
            {
            case ER_ACCESS_DENIED_ERROR:
            case ER_DBACCESS_DENIED_ERROR:
            case ER_ACCESS_DENIED_NO_PASSWORD_ERROR:
                break;

            default:
                rval = true;
                break;
            }
        }
        else if (mxs_mysql_query(mondb->con, query.c_str()) != 0)
        {
            switch (mysql_errno(mondb->con))
            {
            case ER_TABLEACCESS_DENIED_ERROR:
            case ER_COLUMNACCESS_DENIED_ERROR:
            case ER_SPECIFIC_ACCESS_DENIED_ERROR:
            case ER_PROCACCESS_DENIED_ERROR:
            case ER_KILL_DENIED_ERROR:
                rval = false;
                break;

            default:
                rval = true;
                break;
            }

            MXS_ERROR("[%s] Failed to execute query '%s' with user '%s'. MySQL error message: %s",
                      m_name, query.c_str(), m_settings.conn_settings.username.c_str(),
                      mysql_error(mondb->con));
        }
        else
        {
            rval = true;
            MYSQL_RES* res = mysql_use_result(mondb->con);
            if (res == NULL)
            {
                MXS_ERROR("[%s] Result retrieval failed when checking monitor permissions: %s",
                          monitor->m_name,
                          mysql_error(mondb->con));
            }
            else
            {
                mysql_free_result(res);
            }
        }
    }

    MXS_FREE(dpasswd);
    return rval;
}

void monitor_stash_current_status(MXS_MONITORED_SERVER* ptr)
{
    ptr->mon_prev_status = ptr->server->status;
    ptr->pending_status = ptr->server->status;
}

void monitor_set_pending_status(MXS_MONITORED_SERVER* ptr, uint64_t bit)
{
    ptr->pending_status |= bit;
}

void monitor_clear_pending_status(MXS_MONITORED_SERVER* ptr, uint64_t bit)
{
    ptr->pending_status &= ~bit;
}

/*
 * Determine a monitor event, defined by the difference between the old
 * status of a server and the new status.
 *
 * @param   node                The monitor server data for a particular server
 * @result  monitor_event_t     A monitor event (enum)
 *
 * @note This function must only be called from mon_process_state_changes
 */
static mxs_monitor_event_t mon_get_event_type(MXS_MONITORED_SERVER* node)
{
    typedef enum
    {
        DOWN_EVENT,
        UP_EVENT,
        LOSS_EVENT,
        NEW_EVENT,
        UNSUPPORTED_EVENT
    } general_event_type;

    general_event_type event_type = UNSUPPORTED_EVENT;

    uint64_t prev = node->mon_prev_status & all_server_bits;
    uint64_t present = node->server->status & all_server_bits;

    if (prev == present)
    {
        /* This should never happen */
        mxb_assert(false);
        return UNDEFINED_EVENT;
    }

    if ((prev & SERVER_RUNNING) == 0)
    {
        /* The server was not running previously */
        if ((present & SERVER_RUNNING) != 0)
        {
            event_type = UP_EVENT;
        }
        else
        {
            /* Otherwise, was not running and still is not running. This should never happen. */
            mxb_assert(false);
        }
    }
    else
    {
        /* Previous state must have been running */
        if ((present & SERVER_RUNNING) == 0)
        {
            event_type = DOWN_EVENT;
        }
        else
        {
            /** These are used to detect whether we actually lost something or
             * just transitioned from one state to another */
            uint64_t prev_bits = prev & (SERVER_MASTER | SERVER_SLAVE);
            uint64_t present_bits = present & (SERVER_MASTER | SERVER_SLAVE);

            /* Was running and still is */
            if ((!prev_bits || !present_bits || prev_bits == present_bits)
                && (prev & server_type_bits))
            {
                /* We used to know what kind of server it was */
                event_type = LOSS_EVENT;
            }
            else
            {
                /* We didn't know what kind of server it was, now we do*/
                event_type = NEW_EVENT;
            }
        }
    }

    mxs_monitor_event_t rval = UNDEFINED_EVENT;

    switch (event_type)
    {
    case UP_EVENT:
        rval = (present & SERVER_MASTER) ? MASTER_UP_EVENT :
            (present & SERVER_SLAVE) ? SLAVE_UP_EVENT :
            (present
             & SERVER_JOINED) ? SYNCED_UP_EVENT :
            (present
             & SERVER_NDB) ?
            NDB_UP_EVENT
                           :
            SERVER_UP_EVENT;
        break;

    case DOWN_EVENT:
        rval = (prev & SERVER_MASTER) ? MASTER_DOWN_EVENT :
            (prev & SERVER_SLAVE) ? SLAVE_DOWN_EVENT :
            (prev & SERVER_JOINED) ? SYNCED_DOWN_EVENT :
            (prev
             & SERVER_NDB) ?
            NDB_DOWN_EVENT
                           :
            SERVER_DOWN_EVENT;
        break;

    case LOSS_EVENT:
        rval = (prev & SERVER_MASTER) ? LOST_MASTER_EVENT :
            (prev & SERVER_SLAVE) ? LOST_SLAVE_EVENT :
            (prev & SERVER_JOINED) ? LOST_SYNCED_EVENT :
            (prev
             & SERVER_NDB) ?
            LOST_NDB_EVENT
                           :
            UNDEFINED_EVENT;
        break;

    case NEW_EVENT:
        rval = (present & SERVER_MASTER) ? NEW_MASTER_EVENT :
            (present & SERVER_SLAVE) ? NEW_SLAVE_EVENT :
            (present
             & SERVER_JOINED) ? NEW_SYNCED_EVENT :
            (present
             & SERVER_NDB) ?
            NEW_NDB_EVENT
                           :
            UNDEFINED_EVENT;
        break;

    default:
        /* This should never happen */
        mxb_assert(false);
        break;
    }

    mxb_assert(rval != UNDEFINED_EVENT);
    return rval;
}

const char* mon_get_event_name(mxs_monitor_event_t event)
{
    for (int i = 0; mxs_monitor_event_enum_values[i].name; i++)
    {
        if (mxs_monitor_event_enum_values[i].enum_value == event)
        {
            return mxs_monitor_event_enum_values[i].name;
        }
    }

    mxb_assert(!true);
    return "undefined_event";
}

/*
 * Given a monitor event (enum) provide a text string equivalent
 * @param   node    The monitor server data whose event is wanted
 * @result  string  The name of the monitor event for the server
 */
static const char* mon_get_event_name(MXS_MONITORED_SERVER* node)
{
    return mon_get_event_name((mxs_monitor_event_t)node->server->last_event);
}

void Monitor::append_node_names(char* dest, int len, int status, credentials_approach_t approach)
{
    const char* separator = "";
    // Some extra space for port and separator
    char arr[SERVER::MAX_MONUSER_LEN + SERVER::MAX_MONPW_LEN + SERVER::MAX_ADDRESS_LEN + 64];
    dest[0] = '\0';

    for (auto iter = m_servers.begin(); iter != m_servers.end() && len; ++iter)
    {
        Server* server = static_cast<Server*>((*iter)->server);
        if (status == 0 || server->status & status)
        {
            if (approach == CREDENTIALS_EXCLUDE)
            {
                snprintf(arr,
                         sizeof(arr),
                         "%s[%s]:%d",
                         separator,
                         server->address,
                         server->port);
            }
            else
            {
                string user = m_settings.conn_settings.username;
                string password = m_settings.conn_settings.password;
                string server_specific_monuser = server->monitor_user();
                if (!server_specific_monuser.empty())
                {
                    user = server_specific_monuser;
                    password = server->monitor_password();
                }

                snprintf(arr,
                         sizeof(arr),
                         "%s%s:%s@[%s]:%d",
                         separator,
                         user.c_str(),
                         password.c_str(),
                         server->address,
                         server->port);
            }

            separator = ",";
            int arrlen = strlen(arr);

            if (arrlen < len)
            {
                strcat(dest, arr);
                len -= arrlen;
            }
        }
    }
}

/**
 * Check if current monitored server status has changed
 *
 * @param mon_srv       The monitored server
 * @return              true if status has changed or false
 */
bool mon_status_changed(MXS_MONITORED_SERVER* mon_srv)
{
    bool rval = false;

    /* Previous status is -1 if not yet set */
    if (mon_srv->mon_prev_status != static_cast<uint64_t>(-1))
    {

        uint64_t old_status = mon_srv->mon_prev_status & all_server_bits;
        uint64_t new_status = mon_srv->server->status & all_server_bits;

        /**
         * The state has changed if the relevant state bits are not the same,
         * the server is either running, stopping or starting and the server is
         * not going into maintenance or coming out of it
         */
        if (old_status != new_status
            && ((old_status | new_status) & SERVER_MAINT) == 0
            && ((old_status | new_status) & SERVER_RUNNING) == SERVER_RUNNING)
        {
            rval = true;
        }
    }

    return rval;
}

/**
 * Check if current monitored server has a loggable failure status
 *
 * @param mon_srv       The monitored server
 * @return              true if failed status can be logged or false
 */
bool mon_print_fail_status(MXS_MONITORED_SERVER* mon_srv)
{
    return mon_srv->server->is_down() && mon_srv->mon_err_count == 0;
}

static MXS_MONITORED_SERVER* find_parent_node(const std::vector<MXS_MONITORED_SERVER*>& servers,
                                              MXS_MONITORED_SERVER* target)
{
    MXS_MONITORED_SERVER* rval = NULL;

    if (target->server->master_id > 0)
    {
        for (MXS_MONITORED_SERVER* node : servers)
        {
            if (node->server->node_id == target->server->master_id)
            {
                rval = node;
                break;
            }
        }
    }

    return rval;
}

static std::string child_nodes(const std::vector<MXS_MONITORED_SERVER*>& servers,
                               MXS_MONITORED_SERVER* parent)
{
    std::stringstream ss;

    if (parent->server->node_id > 0)
    {
        bool have_content = false;

        for (MXS_MONITORED_SERVER* node : servers)
        {
            if (node->server->master_id == parent->server->node_id)
            {
                if (have_content)
                {
                    ss << ",";
                }

                ss << "[" << node->server->address << "]:" << node->server->port;
                have_content = true;
            }
        }
    }

    return ss.str();
}

int Monitor::launch_command(MXS_MONITORED_SERVER* ptr, EXTERNCMD* cmd)
{
    if (externcmd_matches(cmd, "$INITIATOR"))
    {
        char initiator[strlen(ptr->server->address) + 24];      // Extra space for port
        snprintf(initiator, sizeof(initiator), "[%s]:%d", ptr->server->address, ptr->server->port);
        externcmd_substitute_arg(cmd, "[$]INITIATOR", initiator);
    }

    if (externcmd_matches(cmd, "$PARENT"))
    {
        std::stringstream ss;
        MXS_MONITORED_SERVER* parent = find_parent_node(m_servers, ptr);

        if (parent)
        {
            ss << "[" << parent->server->address << "]:" << parent->server->port;
        }
        externcmd_substitute_arg(cmd, "[$]PARENT", ss.str().c_str());
    }

    if (externcmd_matches(cmd, "$CHILDREN"))
    {
        externcmd_substitute_arg(cmd, "[$]CHILDREN", child_nodes(m_servers, ptr).c_str());
    }

    if (externcmd_matches(cmd, "$EVENT"))
    {
        externcmd_substitute_arg(cmd, "[$]EVENT", mon_get_event_name(ptr));
    }

    char nodelist[PATH_MAX + MON_ARG_MAX + 1] = {'\0'};

    if (externcmd_matches(cmd, "$CREDENTIALS"))
    {
        // We provide the credentials for _all_ servers.
        append_node_names(nodelist, sizeof(nodelist), 0, CREDENTIALS_INCLUDE);
        externcmd_substitute_arg(cmd, "[$]CREDENTIALS", nodelist);
    }

    if (externcmd_matches(cmd, "$NODELIST"))
    {
        append_node_names(nodelist, sizeof(nodelist), SERVER_RUNNING);
        externcmd_substitute_arg(cmd, "[$]NODELIST", nodelist);
    }

    if (externcmd_matches(cmd, "$LIST"))
    {
        append_node_names(nodelist, sizeof(nodelist), 0);
        externcmd_substitute_arg(cmd, "[$]LIST", nodelist);
    }

    if (externcmd_matches(cmd, "$MASTERLIST"))
    {
        append_node_names(nodelist, sizeof(nodelist), SERVER_MASTER);
        externcmd_substitute_arg(cmd, "[$]MASTERLIST", nodelist);
    }

    if (externcmd_matches(cmd, "$SLAVELIST"))
    {
        append_node_names(nodelist, sizeof(nodelist), SERVER_SLAVE);
        externcmd_substitute_arg(cmd, "[$]SLAVELIST", nodelist);
    }

    if (externcmd_matches(cmd, "$SYNCEDLIST"))
    {
        append_node_names(nodelist, sizeof(nodelist), SERVER_JOINED);
        externcmd_substitute_arg(cmd, "[$]SYNCEDLIST", nodelist);
    }

    int rv = externcmd_execute(cmd);

    if (rv)
    {
        if (rv == -1)
        {
            // Internal error
            MXS_ERROR("Failed to execute script '%s' on server state change event '%s'",
                      cmd->argv[0],
                      mon_get_event_name(ptr));
        }
        else
        {
            // Script returned a non-zero value
            MXS_ERROR("Script '%s' returned %d on event '%s'",
                      cmd->argv[0],
                      rv,
                      mon_get_event_name(ptr));
        }
    }
    else
    {
        mxb_assert(cmd->argv != NULL && cmd->argv[0] != NULL);
        // Construct a string with the script + arguments
        char* scriptStr = NULL;
        int totalStrLen = 0;
        bool memError = false;
        for (int i = 0; cmd->argv[i]; i++)
        {
            totalStrLen += strlen(cmd->argv[i]) + 1;    // +1 for space and one \0
        }
        int spaceRemaining = totalStrLen;
        if ((scriptStr = (char*)MXS_CALLOC(totalStrLen, sizeof(char))) != NULL)
        {
            char* currentPos = scriptStr;
            // The script name should not begin with a space
            int len = snprintf(currentPos, spaceRemaining, "%s", cmd->argv[0]);
            currentPos += len;
            spaceRemaining -= len;

            for (int i = 1; cmd->argv[i]; i++)
            {
                if ((cmd->argv[i])[0] == '\0')
                {
                    continue;   // Empty argument, print nothing
                }
                len = snprintf(currentPos, spaceRemaining, " %s", cmd->argv[i]);
                currentPos += len;
                spaceRemaining -= len;
            }
            mxb_assert(spaceRemaining > 0);
            *currentPos = '\0';
        }
        else
        {
            memError = true;
            scriptStr = cmd->argv[0];   // print at least something
        }

        MXS_NOTICE("Executed monitor script '%s' on event '%s'",
                   scriptStr,
                   mon_get_event_name(ptr));

        if (!memError)
        {
            MXS_FREE(scriptStr);
        }
    }

    return rv;
}

int Monitor::launch_script(MXS_MONITORED_SERVER* ptr)
{
    const char* script = m_settings.script.c_str();
    char arg[strlen(script) + 1];
    strcpy(arg, script);

    EXTERNCMD* cmd = externcmd_allocate(arg, m_settings.script_timeout);

    if (cmd == NULL)
    {
        MXS_ERROR("Failed to initialize script '%s'. See previous errors for the "
                  "cause of this failure.",
                  script);
        return -1;
    }

    int rv = launch_command(ptr, cmd);

    externcmd_free(cmd);

    return rv;
}

mxs_connect_result_t mon_ping_or_connect_to_db(const MXS_MONITORED_SERVER::ConnectionSettings& sett,
                                               SERVER& server, MYSQL** ppConn)
{
    mxb_assert(ppConn);
    auto pConn = *ppConn;
    if (pConn)
    {
        /** Return if the connection is OK */
        if (mysql_ping(pConn) == 0)
        {
            return MONITOR_CONN_EXISTING_OK;
        }
        /** Otherwise close the handle. */
        mysql_close(pConn);
    }

    mxs_connect_result_t conn_result = MONITOR_CONN_REFUSED;
    if ((pConn = mysql_init(NULL)) != nullptr)
    {
        string uname = sett.username;
        string passwd = sett.password;
        const Server& srv = static_cast<const Server&>(server); // Clean this up later.
        string server_specific_monuser = srv.monitor_user();
        if (!server_specific_monuser.empty())
        {
            uname = server_specific_monuser;
            passwd = srv.monitor_password();
        }
        char* dpwd = decrypt_password(passwd.c_str());

        mysql_optionsv(pConn, MYSQL_OPT_CONNECT_TIMEOUT, &sett.connect_timeout);
        mysql_optionsv(pConn, MYSQL_OPT_READ_TIMEOUT, &sett.read_timeout);
        mysql_optionsv(pConn, MYSQL_OPT_WRITE_TIMEOUT, &sett.write_timeout);
        mysql_optionsv(pConn, MYSQL_PLUGIN_DIR, get_connector_plugindir());

        time_t start = 0;
        time_t end = 0;
        for (int i = 0; i < sett.connect_attempts; i++)
        {
            start = time(NULL);
            bool result = (mxs_mysql_real_connect(pConn, &server, uname.c_str(), dpwd) != NULL);
            end = time(NULL);

            if (result)
            {
                conn_result = MONITOR_CONN_NEWCONN_OK;
                break;
            }
        }

        if (conn_result == MONITOR_CONN_REFUSED && difftime(end, start) >= sett.connect_timeout)
        {
            conn_result = MONITOR_CONN_TIMEOUT;
        }
        MXS_FREE(dpwd);
    }

    *ppConn = pConn;
    return conn_result;
}

mxs_connect_result_t MXS_MONITORED_SERVER::ping_or_connect(const ConnectionSettings& settings)
{
    return mon_ping_or_connect_to_db(settings, *server, &con);
}

/**
 * Is the return value one of the 'OK' values.
 *
 * @param connect_result Return value of mon_ping_or_connect_to_db
 * @return True of connection is ok
 */
bool mon_connection_is_ok(mxs_connect_result_t connect_result)
{
    return connect_result == MONITOR_CONN_EXISTING_OK || connect_result == MONITOR_CONN_NEWCONN_OK;
}

/**
 * Log an error about the failure to connect to a backend server and why it happened.
 *
 * @param database Backend database
 * @param rval Return value of mon_ping_or_connect_to_db
 */
void mon_log_connect_error(MXS_MONITORED_SERVER* database, mxs_connect_result_t rval)
{
    mxb_assert(!mon_connection_is_ok(rval) && database);
    const char TIMED_OUT[] = "Monitor timed out when connecting to server %s[%s:%d] : '%s'";
    const char REFUSED[] = "Monitor was unable to connect to server %s[%s:%d] : '%s'";
    auto srv = database->server;
    MXS_ERROR(rval == MONITOR_CONN_TIMEOUT ? TIMED_OUT : REFUSED,
              srv->name(),
              srv->address,
              srv->port,
              mysql_error(database->con));
}

static void mon_log_state_change(MXS_MONITORED_SERVER* ptr)
{
    string prev = SERVER::status_to_string(ptr->mon_prev_status);
    string next = ptr->server->status_string();
    MXS_NOTICE("Server changed state: %s[%s:%u]: %s. [%s] -> [%s]",
               ptr->server->name(), ptr->server->address, ptr->server->port,
               mon_get_event_name(ptr),
               prev.c_str(), next.c_str());
}

Monitor* monitor_server_in_use(const SERVER* server)
{
    Monitor* rval = nullptr;
    this_unit.foreach_monitor([&rval, server](Monitor* monitor) {
        Guard guard(monitor->m_lock);
        if (monitor->m_active)
        {
            for (MXS_MONITORED_SERVER* db : monitor->m_servers)
            {
                if (db->server == server)
                {
                    rval = monitor;
                    break;
                }
            }
        }
        return (rval == nullptr);
    });
    return rval;
}

static bool create_monitor_config(const Monitor* monitor, const char* filename)
{
    int file = open(filename, O_EXCL | O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

    if (file == -1)
    {
        MXS_ERROR("Failed to open file '%s' when serializing monitor '%s': %d, %s",
                  filename,
                  monitor->m_name,
                  errno,
                  mxs_strerror(errno));
        return false;
    }

    {
        Guard guard(monitor->m_lock);
        dprintf(file, "[%s]\n", monitor->m_name);
        dprintf(file, "%s=monitor\n", CN_TYPE);

        if (!monitor->m_servers.empty())
        {
            dprintf(file, "%s=", CN_SERVERS);
            for (MXS_MONITORED_SERVER* db : monitor->m_servers)
            {
                if (db != monitor->m_servers[0])
                {
                    dprintf(file, ",");
                }
                dprintf(file, "%s", db->server->name());
            }
            dprintf(file, "\n");
        }

        const MXS_MODULE* mod = get_module(monitor->m_module.c_str(), NULL);
        mxb_assert(mod);

        dump_param_list(file,
                        &monitor->parameters,
                        {CN_TYPE, CN_SERVERS},
                        config_monitor_params,
                        mod->parameters);
    }

    close(file);
    return true;
}

bool monitor_serialize(const Monitor* monitor)
{
    bool rval = false;
    char filename[PATH_MAX];
    snprintf(filename,
             sizeof(filename),
             "%s/%s.cnf.tmp",
             get_config_persistdir(),
             monitor->m_name);

    if (unlink(filename) == -1 && errno != ENOENT)
    {
        MXS_ERROR("Failed to remove temporary monitor configuration at '%s': %d, %s",
                  filename,
                  errno,
                  mxs_strerror(errno));
    }
    else if (create_monitor_config(monitor, filename))
    {
        char final_filename[PATH_MAX];
        strcpy(final_filename, filename);

        char* dot = strrchr(final_filename, '.');
        mxb_assert(dot);
        *dot = '\0';

        if (rename(filename, final_filename) == 0)
        {
            rval = true;
        }
        else
        {
            MXS_ERROR("Failed to rename temporary monitor configuration at '%s': %d, %s",
                      filename,
                      errno,
                      mxs_strerror(errno));
        }
    }

    return rval;
}

void mon_hangup_failed_servers(Monitor* monitor)
{
    for (MXS_MONITORED_SERVER* ptr : monitor->m_servers)
    {
        if (mon_status_changed(ptr) && (!(ptr->server->is_usable()) || !(ptr->server->is_in_cluster())))
        {
            dcb_hangup_foreach(ptr->server);
        }
    }
}

void mon_report_query_error(MXS_MONITORED_SERVER* db)
{
    MXS_ERROR("Failed to execute query on server '%s' ([%s]:%d): %s",
              db->server->name(),
              db->server->address,
              db->server->port,
              mysql_error(db->con));
}

/**
 * Check if admin is requesting setting or clearing maintenance status on the server and act accordingly.
 * Should be called at the beginning of a monitor loop.
 *
 * @param monitor The target monitor
 */
void monitor_check_maintenance_requests(Monitor* monitor)
{
    /* In theory, the admin may be modifying the server maintenance status during this function. The overall
     * maintenance flag should be read-written atomically to prevent missing a value. */
    int flags_changed = atomic_exchange_int(&monitor->check_status_flag, Monitor::STATUS_FLAG_NOCHECK);
    if (flags_changed != Monitor::STATUS_FLAG_NOCHECK)
    {
        for (auto ptr : monitor->m_servers)
        {
            // The only server status bit the admin may change is the [Maintenance] bit.
            int admin_msg = atomic_exchange_int(&ptr->status_request,
                                                MXS_MONITORED_SERVER::NO_CHANGE);

            switch (admin_msg)
            {
            case MXS_MONITORED_SERVER::MAINT_ON:
                // TODO: Change to writing MONITORED_SERVER->pending status instead once cleanup done.
                ptr->server->set_status(SERVER_MAINT);
                break;

            case MXS_MONITORED_SERVER::MAINT_OFF:
                ptr->server->clear_status(SERVER_MAINT);
                break;

            case MXS_MONITORED_SERVER::BEING_DRAINED_ON:
                ptr->server->set_status(SERVER_BEING_DRAINED);
                break;

            case MXS_MONITORED_SERVER::BEING_DRAINED_OFF:
                ptr->server->clear_status(SERVER_BEING_DRAINED);
                break;

            case MXS_MONITORED_SERVER::NO_CHANGE:
                break;

            default:
                mxb_assert(!true);
            }
        }
    }
}

void Monitor::detect_handle_state_changes()
{
    bool master_down = false;
    bool master_up = false;

    for (MXS_MONITORED_SERVER* ptr : m_servers)
    {
        if (mon_status_changed(ptr))
        {
            /**
             * The last executed event will be needed if a passive MaxScale is
             * promoted to an active one and the last event that occurred on
             * a server was a master_down event.
             *
             * In this case, a failover script should be called if no master_up
             * or new_master events are triggered within a pre-defined time limit.
             */
            mxs_monitor_event_t event = mon_get_event_type(ptr);
            ptr->server->last_event = event;
            ptr->server->triggered_at = mxs_clock();
            mon_log_state_change(ptr);

            if (event == MASTER_DOWN_EVENT)
            {
                master_down = true;
            }
            else if (event == MASTER_UP_EVENT || event == NEW_MASTER_EVENT)
            {
                master_up = true;
            }

            if (!m_settings.script.empty() && (event & m_settings.events))
            {
                launch_script(ptr);
            }
        }
    }

    if (master_down && master_up)
    {
        MXS_NOTICE("Master switch detected: lost a master and gained a new one");
    }
}

static const char* monitor_state_to_string(monitor_state_t state)
{
    switch (state)
    {
    case MONITOR_STATE_RUNNING:
        return "Running";

    case MONITOR_STATE_STOPPED:
        return "Stopped";

    default:
        mxb_assert(false);
        return "Unknown";
    }
}

json_t* monitor_parameters_to_json(const Monitor* monitor)
{
    json_t* rval = json_object();
    const MXS_MODULE* mod = get_module(monitor->m_module.c_str(), MODULE_MONITOR);
    config_add_module_params_json(&monitor->parameters,
                                  {CN_TYPE, CN_MODULE, CN_SERVERS},
                                  config_monitor_params,
                                  mod->parameters,
                                  rval);
    return rval;
}

json_t* monitor_json_data(const Monitor* monitor, const char* host)
{
    json_t* rval = json_object();
    json_t* attr = json_object();
    json_t* rel = json_object();

    {
        Guard guard(monitor->m_lock);
        json_object_set_new(rval, CN_ID, json_string(monitor->m_name));
        json_object_set_new(rval, CN_TYPE, json_string(CN_MONITORS));

        json_object_set_new(attr, CN_MODULE, json_string(monitor->m_module.c_str()));
        json_object_set_new(attr, CN_STATE, json_string(monitor_state_to_string(monitor->state())));
        json_object_set_new(attr, CN_TICKS, json_integer(monitor->m_ticks));

        /** Monitor parameters */
        json_object_set_new(attr, CN_PARAMETERS, monitor_parameters_to_json(monitor));

        if (monitor->state() == MONITOR_STATE_RUNNING)
        {
            json_t* diag = monitor->diagnostics_json();
            if (diag)
            {
                json_object_set_new(attr, CN_MONITOR_DIAGNOSTICS, diag);
            }
        }

        if (!monitor->m_servers.empty())
        {
            json_t* mon_rel = mxs_json_relationship(host, MXS_JSON_API_SERVERS);
            for (MXS_MONITORED_SERVER* db : monitor->m_servers)
            {
                mxs_json_add_relation(mon_rel, db->server->name(), CN_SERVERS);
            }
            json_object_set_new(rel, CN_SERVERS, mon_rel);
        }
    }

    json_object_set_new(rval, CN_RELATIONSHIPS, rel);
    json_object_set_new(rval, CN_ATTRIBUTES, attr);
    json_object_set_new(rval, CN_LINKS, mxs_json_self_link(host, CN_MONITORS, monitor->m_name));
    return rval;
}

json_t* monitor_to_json(const Monitor* monitor, const char* host)
{
    string self = MXS_JSON_API_MONITORS;
    self += monitor->m_name;
    return mxs_json_resource(host, self.c_str(), monitor_json_data(monitor, host));
}

json_t* monitor_list_to_json(const char* host)
{
    json_t* rval = json_array();
    this_unit.foreach_monitor([rval, host](Monitor* mon) {
        if (mon->m_active)
        {
            json_t* json = monitor_json_data(mon, host);

            if (json)
            {
                json_array_append_new(rval, json);
            }
        }
        return true;
    });

    return mxs_json_resource(host, MXS_JSON_API_MONITORS, rval);
}

json_t* monitor_relations_to_server(const SERVER* server, const char* host)
{
    std::vector<std::string> names;
    this_unit.foreach_monitor([&names, server](Monitor* mon) {
        Guard guard(mon->m_lock);
        if (mon->m_active)
        {
            for (MXS_MONITORED_SERVER* db : mon->m_servers)
            {
                if (db->server == server)
                {
                    names.push_back(mon->m_name);
                    break;
                }
            }
        }
        return true;
    });

    json_t* rel = NULL;
    if (!names.empty())
    {
        rel = mxs_json_relationship(host, MXS_JSON_API_MONITORS);

        for (std::vector<std::string>::iterator it = names.begin();
             it != names.end(); it++)
        {
            mxs_json_add_relation(rel, it->c_str(), CN_MONITORS);
        }
    }

    return rel;
}

static const char journal_name[] = "monitor.dat";
static const char journal_template[] = "%s/%s/%s";

/**
 * @brief Remove .tmp suffix and rename file
 *
 * @param src File to rename
 * @return True if file was successfully renamed
 */
static bool rename_tmp_file(Monitor* monitor, const char* src)
{
    bool rval = true;
    char dest[PATH_MAX + 1];
    snprintf(dest, sizeof(dest), journal_template, get_datadir(), monitor->m_name, journal_name);

    if (rename(src, dest) == -1)
    {
        rval = false;
        MXS_ERROR("Failed to rename journal file '%s' to '%s': %d, %s",
                  src,
                  dest,
                  errno,
                  mxs_strerror(errno));
    }

    return rval;
}

/**
 * @brief Open temporary file
 *
 * @param monitor Monitor
 * @param path Output where the path is stored
 * @return Opened file or NULL on error
 */
static FILE* open_tmp_file(Monitor* monitor, char* path)
{
    int nbytes = snprintf(path, PATH_MAX, journal_template, get_datadir(), monitor->m_name, "");
    int max_bytes = PATH_MAX - (int)sizeof(journal_name);
    FILE* rval = NULL;

    if (nbytes < max_bytes && mxs_mkdir_all(path, 0744))
    {
        strcat(path, journal_name);
        strcat(path, "XXXXXX");
        int fd = mkstemp(path);

        if (fd == -1)
        {
            MXS_ERROR("Failed to open file '%s': %d, %s", path, errno, mxs_strerror(errno));
        }
        else
        {
            rval = fdopen(fd, "w");
        }
    }
    else
    {
        MXS_ERROR("Path is too long: %d characters exceeds the maximum path "
                  "length of %d bytes",
                  nbytes,
                  max_bytes);
    }

    return rval;
}

/**
 * @brief Store server data to in-memory buffer
 *
 * @param monitor Monitor
 * @param data Pointer to in-memory buffer used for storage, should be at least
 *             PATH_MAX bytes long
 * @param size Size of @c data
 */
static void store_data(Monitor* monitor, MXS_MONITORED_SERVER* master, uint8_t* data, uint32_t size)
{
    uint8_t* ptr = data;

    /** Store the data length */
    mxb_assert(sizeof(size) == MMB_LEN_BYTES);
    ptr = mxs_set_byte4(ptr, size);

    /** Then the schema version */
    *ptr++ = MMB_SCHEMA_VERSION;

    /** Store the states of all servers */
    for (MXS_MONITORED_SERVER* db : monitor->m_servers)
    {
        *ptr++ = (char)SVT_SERVER;                              // Value type
        memcpy(ptr, db->server->name(), strlen(db->server->name()));// Name of the server
        ptr += strlen(db->server->name());
        *ptr++ = '\0';      // Null-terminate the string

        auto status = db->server->status;
        static_assert(sizeof(status) == MMB_LEN_SERVER_STATUS,
                      "Status size should be MMB_LEN_SERVER_STATUS bytes");
        ptr = maxscale::set_byteN(ptr, status, MMB_LEN_SERVER_STATUS);
    }

    /** Store the current root master if we have one */
    if (master)
    {
        *ptr++ = (char)SVT_MASTER;
        memcpy(ptr, master->server->name(), strlen(master->server->name()));
        ptr += strlen(master->server->name());
        *ptr++ = '\0';      // Null-terminate the string
    }

    /** Calculate the CRC32 for the complete payload minus the CRC32 bytes */
    uint32_t crc = crc32(0L, NULL, 0);
    crc = crc32(crc, (uint8_t*)data + MMB_LEN_BYTES, size - MMB_LEN_CRC32);
    mxb_assert(sizeof(crc) == MMB_LEN_CRC32);

    ptr = mxs_set_byte4(ptr, crc);
    mxb_assert(ptr - data == size + MMB_LEN_BYTES);
}

static int get_data_file_path(const Monitor* monitor, char* path)
{
    int rv = snprintf(path, PATH_MAX, journal_template, get_datadir(), monitor->m_name, journal_name);
    return rv;
}

/**
 * @brief Open stored journal file
 *
 * @param monitor Monitor to reload
 * @param path Output where path is stored
 * @return Opened file or NULL on error
 */
static FILE* open_data_file(Monitor* monitor, char* path)
{
    FILE* rval = NULL;
    int nbytes = get_data_file_path(monitor, path);

    if (nbytes < PATH_MAX)
    {
        if ((rval = fopen(path, "rb")) == NULL && errno != ENOENT)
        {
            MXS_ERROR("Failed to open journal file: %d, %s", errno, mxs_strerror(errno));
        }
    }
    else
    {
        MXS_ERROR("Path is too long: %d characters exceeds the maximum path "
                  "length of %d bytes",
                  nbytes,
                  PATH_MAX);
    }

    return rval;
}

/**
 * Check that memory area contains a null terminator
 */
static bool has_null_terminator(const char* data, const char* end)
{
    while (data < end)
    {
        if (*data == '\0')
        {
            return true;
        }
        data++;
    }

    return false;
}

/**
 * Process a generic server
 */
static const char* process_server(Monitor* monitor, const char* data, const char* end)
{
    for (MXS_MONITORED_SERVER* db : monitor->m_servers)
    {
        if (strcmp(db->server->name(), data) == 0)
        {
            const unsigned char* sptr = (unsigned char*)strchr(data, '\0');
            mxb_assert(sptr);
            sptr++;

            uint64_t status = maxscale::get_byteN(sptr, MMB_LEN_SERVER_STATUS);
            db->mon_prev_status = status;
            db->server->set_status(status);
            monitor_set_pending_status(db, status);
            break;
        }
    }

    data += strlen(data) + 1 + MMB_LEN_SERVER_STATUS;

    return data;
}

/**
 * Process a master
 */
static const char* process_master(Monitor* monitor,
                                  MXS_MONITORED_SERVER** master,
                                  const char* data,
                                  const char* end)
{
    if (master)
    {
        for (MXS_MONITORED_SERVER* db : monitor->m_servers)
        {
            if (strcmp(db->server->name(), data) == 0)
            {
                *master = db;
                break;
            }
        }
    }

    data += strlen(data) + 1;

    return data;
}

/**
 * Check that the calculated CRC32 matches the one stored on disk
 */
static bool check_crc32(const uint8_t* data, uint32_t size, const uint8_t* crc_ptr)
{
    uint32_t crc = mxs_get_byte4(crc_ptr);
    uint32_t calculated_crc = crc32(0L, NULL, 0);
    calculated_crc = crc32(calculated_crc, data, size);
    return calculated_crc == crc;
}

/**
 * Process the stored journal data
 */
static bool process_data_file(Monitor* monitor,
                              MXS_MONITORED_SERVER** master,
                              const char* data,
                              const char* crc_ptr)
{
    const char* ptr = data;
    MXB_AT_DEBUG(const char* prevptr = ptr);

    while (ptr < crc_ptr)
    {
        /** All values contain a null terminated string */
        if (!has_null_terminator(ptr, crc_ptr))
        {
            MXS_ERROR("Possible corrupted journal file (no null terminator found). Ignoring.");
            return false;
        }

        stored_value_type type = (stored_value_type)ptr[0];
        ptr += MMB_LEN_VALUE_TYPE;

        switch (type)
        {
        case SVT_SERVER:
            ptr = process_server(monitor, ptr, crc_ptr);
            break;

        case SVT_MASTER:
            ptr = process_master(monitor, master, ptr, crc_ptr);
            break;

        default:
            MXS_ERROR("Possible corrupted journal file (unknown stored value). Ignoring.");
            return false;
        }
        mxb_assert(prevptr != ptr);
        MXB_AT_DEBUG(prevptr = ptr);
    }

    mxb_assert(ptr == crc_ptr);
    return true;
}

void store_server_journal(Monitor* monitor, MXS_MONITORED_SERVER* master)
{
    /** Calculate how much memory we need to allocate */
    uint32_t size = MMB_LEN_SCHEMA_VERSION + MMB_LEN_CRC32;

    for (MXS_MONITORED_SERVER* db : monitor->m_servers)
    {
        /** Each server is stored as a type byte and a null-terminated string
         * followed by eight byte server status. */
        size += MMB_LEN_VALUE_TYPE + strlen(db->server->name()) + 1 + MMB_LEN_SERVER_STATUS;
    }

    if (master)
    {
        /** The master server name is stored as a null terminated string */
        size += MMB_LEN_VALUE_TYPE + strlen(master->server->name()) + 1;
    }

    /** 4 bytes for file length, 1 byte for schema version and 4 bytes for CRC32 */
    uint32_t buffer_size = size + MMB_LEN_BYTES;
    uint8_t* data = (uint8_t*)MXS_MALLOC(buffer_size);
    char path[PATH_MAX + 1];

    if (data)
    {
        /** Store the data in memory first and compare the current hash to
         * the hash of the last stored journal. This isn't a fool-proof
         * method of detecting changes but any failures are mainly of
         * theoretical nature. */
        store_data(monitor, master, data, size);
        uint8_t hash[SHA_DIGEST_LENGTH];
        SHA1(data, size, hash);

        if (memcmp(monitor->m_journal_hash, hash, sizeof(hash)) != 0)
        {
            FILE* file = open_tmp_file(monitor, path);

            if (file)
            {
                /** Write the data to a temp file and rename it to the final name */
                if (fwrite(data, 1, buffer_size, file) == buffer_size && fflush(file) == 0)
                {
                    if (!rename_tmp_file(monitor, path))
                    {
                        unlink(path);
                    }
                    else
                    {
                        memcpy(monitor->m_journal_hash, hash, sizeof(hash));
                    }
                }
                else
                {
                    MXS_ERROR("Failed to write journal data to disk: %d, %s",
                              errno,
                              mxs_strerror(errno));
                }
                fclose(file);
            }
        }
    }
    MXS_FREE(data);
}

void load_server_journal(Monitor* monitor, MXS_MONITORED_SERVER** master)
{
    char path[PATH_MAX];
    FILE* file = open_data_file(monitor, path);

    if (file)
    {
        uint32_t size = 0;
        size_t bytes = fread(&size, 1, MMB_LEN_BYTES, file);
        mxb_assert(sizeof(size) == MMB_LEN_BYTES);

        if (bytes == MMB_LEN_BYTES)
        {
            /** Payload contents:
             *
             * - One byte of schema version
             * - `size - 5` bytes of data
             * - Trailing 4 bytes of CRC32
             */
            char* data = (char*)MXS_MALLOC(size);

            if (data && (bytes = fread(data, 1, size, file)) == size)
            {
                if (*data == MMB_SCHEMA_VERSION)
                {
                    if (check_crc32((uint8_t*)data,
                                    size - MMB_LEN_CRC32,
                                    (uint8_t*)data + size - MMB_LEN_CRC32))
                    {
                        if (process_data_file(monitor,
                                              master,
                                              data + MMB_LEN_SCHEMA_VERSION,
                                              data + size - MMB_LEN_CRC32))
                        {
                            MXS_NOTICE("Loaded server states from journal file: %s", path);
                        }
                    }
                    else
                    {
                        MXS_ERROR("CRC32 mismatch in journal file. Ignoring.");
                    }
                }
                else
                {
                    MXS_ERROR("Unknown journal schema version: %d", (int)*data);
                }
            }
            else if (data)
            {
                if (ferror(file))
                {
                    MXS_ERROR("Failed to read journal file: %d, %s", errno, mxs_strerror(errno));
                }
                else
                {
                    MXS_ERROR("Failed to read journal file: Expected %u bytes, "
                              "read %lu bytes.",
                              size,
                              bytes);
                }
            }
            MXS_FREE(data);
        }
        else
        {
            if (ferror(file))
            {
                MXS_ERROR("Failed to read journal file length: %d, %s",
                          errno,
                          mxs_strerror(errno));
            }
            else
            {
                MXS_ERROR("Failed to read journal file length: Expected %d bytes, "
                          "read %lu bytes.",
                          MMB_LEN_BYTES,
                          bytes);
            }
        }

        fclose(file);
    }
}

static void remove_server_journal(Monitor* monitor)
{
    char path[PATH_MAX];

    if (get_data_file_path(monitor, path) < PATH_MAX)
    {
        unlink(path);
    }
    else
    {
        MXS_ERROR("Path to monitor journal directory is too long.");
    }
}

bool Monitor::journal_is_stale() const
{
    bool is_stale = true;
    char path[PATH_MAX];
    auto max_age = m_settings.journal_max_age;
    if (get_data_file_path(this, path) < PATH_MAX)
    {
        struct stat st;

        if (stat(path, &st) == 0)
        {
            time_t tdiff = time(NULL) - st.st_mtim.tv_sec;

            if (tdiff >= max_age)
            {
                MXS_WARNING("Journal file was created %ld seconds ago. Maximum journal "
                            "age is %ld seconds.",
                            tdiff,
                            max_age);
            }
            else
            {
                is_stale = false;
            }
        }
        else if (errno != ENOENT)
        {
            MXS_ERROR("Failed to inspect journal file: %d, %s", errno, mxs_strerror(errno));
        }
    }
    else
    {
        MXS_ERROR("Path to monitor journal directory is too long.");
    }

    return is_stale;
}

MXS_MONITORED_SERVER* mon_get_monitored_server(const Monitor* mon, SERVER* search_server)
{
    mxb_assert(mon && search_server);
    for (MXS_MONITORED_SERVER* iter : mon->m_servers)
    {
        if (iter->server == search_server)
        {
            return iter;
        }
    }
    return NULL;
}

std::vector<MXS_MONITORED_SERVER*> mon_config_get_servers(const MXS_CONFIG_PARAMETER* params,
                                                          const char* key, const Monitor* mon,
                                                          bool* error_out)
{
    std::vector<MXS_MONITORED_SERVER*> monitored_array;
    // Check that value exists.
    if (!params->contains(key))
    {
        return monitored_array;
    }

    string name_error;
    auto servers = params->get_server_list(key, &name_error);
    if (!servers.empty())
    {
        // All servers in the array must be monitored by the given monitor.
        for (auto elem : servers)
        {
            MXS_MONITORED_SERVER* mon_serv = mon_get_monitored_server(mon, elem);
            if (mon_serv)
            {
                monitored_array.push_back(mon_serv);
            }
            else
            {
                MXS_ERROR("Server '%s' is not monitored by monitor '%s'.", elem->name(), mon->m_name);
                *error_out = true;
            }
        }

        if (monitored_array.size() < servers.size())
        {
            monitored_array.clear();
        }
    }
    else
    {
        MXS_ERROR("Serverlist setting '%s' contains invalid server name '%s'.", key, name_error.c_str());
        *error_out = true;
    }

    return monitored_array;
}

bool Monitor::set_disk_space_threshold(const string& dst_setting)
{
    mxb_assert(state() == MONITOR_STATE_STOPPED);
    SERVER::DiskSpaceLimits new_dst;
    bool rv = config_parse_disk_space_threshold(&new_dst, dst_setting.c_str());
    if (rv)
    {
        m_settings.disk_space_limits = new_dst;
    }
    return rv;
}

namespace
{

const char ERR_CANNOT_MODIFY[] =
    "The server is monitored, so only the maintenance status can be "
    "set/cleared manually. Status was not modified.";
const char WRN_REQUEST_OVERWRITTEN[] =
    "Previous maintenance request was not yet read by the monitor and was overwritten.";
}

bool Monitor::set_server_status(SERVER* srv, int bit, string* errmsg_out)
{
    MXS_MONITORED_SERVER* msrv = mon_get_monitored_server(this, srv);
    mxb_assert(msrv);

    if (!msrv)
    {
        MXS_ERROR("Monitor %s requested to set status of server %s that it does not monitor.",
                  m_name, srv->address);
        return false;
    }

    bool written = false;

    if (state() == MONITOR_STATE_RUNNING)
    {
        /* This server is monitored, in which case modifying any other status bit than Maintenance is
         * disallowed. */
        if (bit & ~(SERVER_MAINT | SERVER_BEING_DRAINED))
        {
            MXS_ERROR(ERR_CANNOT_MODIFY);
            if (errmsg_out)
            {
                *errmsg_out = ERR_CANNOT_MODIFY;
            }
        }
        else
        {
            /* Maintenance and being-drained are set/cleared using a special variable which the
             * monitor reads when starting the next update cycle. */

            int request;
            if (bit & SERVER_MAINT)
            {
                request = MXS_MONITORED_SERVER::MAINT_ON;
            }
            else
            {
                mxb_assert(bit & SERVER_BEING_DRAINED);
                request = MXS_MONITORED_SERVER::BEING_DRAINED_ON;
            }

            int previous_request = atomic_exchange_int(&msrv->status_request, request);
            written = true;
            // Warn if the previous request hasn't been read.
            if (previous_request != MXS_MONITORED_SERVER::NO_CHANGE)
            {
                MXS_WARNING(WRN_REQUEST_OVERWRITTEN);
            }
            // Also set a flag so the next loop happens sooner.
            atomic_store_int(&this->check_status_flag, Monitor::STATUS_FLAG_CHECK);
        }
    }
    else
    {
        /* The monitor is not running, the bit can be set directly */
        srv->set_status(bit);
        written = true;
    }

    return written;
}

bool Monitor::clear_server_status(SERVER* srv, int bit, string* errmsg_out)
{
    MXS_MONITORED_SERVER* msrv = mon_get_monitored_server(this, srv);
    mxb_assert(msrv);

    if (!msrv)
    {
        MXS_ERROR("Monitor %s requested to clear status of server %s that it does not monitor.",
                  m_name, srv->address);
        return false;
    }

    bool written = false;

    if (state() == MONITOR_STATE_RUNNING)
    {
        if (bit & ~(SERVER_MAINT | SERVER_BEING_DRAINED))
        {
            MXS_ERROR(ERR_CANNOT_MODIFY);
            if (errmsg_out)
            {
                *errmsg_out = ERR_CANNOT_MODIFY;
            }
        }
        else
        {
            int request;
            if (bit & SERVER_MAINT)
            {
                request = MXS_MONITORED_SERVER::MAINT_OFF;
            }
            else
            {
                mxb_assert(bit & SERVER_BEING_DRAINED);
                request = MXS_MONITORED_SERVER::BEING_DRAINED_OFF;
            }

            int previous_request = atomic_exchange_int(&msrv->status_request, request);
            written = true;
            // Warn if the previous request hasn't been read.
            if (previous_request != MXS_MONITORED_SERVER::NO_CHANGE)
            {
                MXS_WARNING(WRN_REQUEST_OVERWRITTEN);
            }
            // Also set a flag so the next loop happens sooner.
            atomic_store_int(&this->check_status_flag, Monitor::STATUS_FLAG_CHECK);
        }
    }
    else
    {
        /* The monitor is not running, the bit can be cleared directly */
        srv->clear_status(bit);
        written = true;
    }

    return written;
}

void Monitor::populate_services()
{
    mxb_assert(state() == MONITOR_STATE_STOPPED);

    for (MXS_MONITORED_SERVER* pMs : m_servers)
    {
        service_add_server(this, pMs->server);
    }
}

void monitor_debug_wait()
{
    using namespace std::chrono;
    std::map<Monitor*, uint64_t> ticks;

    // Get tick values for all monitors
    this_unit.foreach_monitor([&ticks](Monitor* mon) {
        ticks[mon] = mxb::atomic::load(&mon->m_ticks);
        return true;
    });

    // Wait for all running monitors to advance at least one tick.
    this_unit.foreach_monitor([&ticks](Monitor* mon) {
            if (mon->state() == MONITOR_STATE_RUNNING)
        {
            auto start = steady_clock::now();
            // A monitor may have been added in between the two foreach-calls (not if config changes are
            // serialized). Check if entry exists.
            if (ticks.count(mon) > 0)
            {
                auto tick = ticks[mon];
                while (mxb::atomic::load(&mon->m_ticks) == tick && (steady_clock::now() - start < seconds(60)))
                {
                    std::this_thread::sleep_for(milliseconds(100));
                }
            }
        }
        return true;
    });
}

namespace maxscale
{

MonitorWorker::MonitorWorker(const string& name, const string& module)
    : Monitor(name, module)
    , m_monitor(this)
    , m_master(NULL)
    , m_thread_running(false)
    , m_shutdown(0)
    , m_checked(false)
    , m_loop_called(get_time_ms())
{
}

MonitorWorker::~MonitorWorker()
{
}

monitor_state_t MonitorWorker::state() const
{
    bool running = (Worker::state() != Worker::STOPPED);

    return running ? MONITOR_STATE_RUNNING : MONITOR_STATE_STOPPED;
}

void MonitorWorker::do_stop()
{
    // This should only be called by monitor_stop(). NULL worker is allowed since the main worker may
    // not exist during program start/stop.
    mxb_assert(mxs_rworker_get_current() == NULL
               || mxs_rworker_get_current() == mxs_rworker_get(MXS_RWORKER_MAIN));
    mxb_assert(Worker::state() != Worker::STOPPED);
    mxb_assert(state() != MONITOR_STATE_STOPPED);
    mxb_assert(m_thread_running.load() == true);

    Worker::shutdown();
    Worker::join();
    m_thread_running.store(false, std::memory_order_release);
}

void MonitorWorker::diagnostics(DCB* pDcb) const
{
}

json_t* MonitorWorker::diagnostics_json() const
{
    return json_object();
}

bool MonitorWorker::start(const MXS_CONFIG_PARAMETER* pParams)
{
    // This should only be called by monitor_start(). NULL worker is allowed since the main worker may
    // not exist during program start/stop.
    mxb_assert(mxs_rworker_get_current() == NULL
               || mxs_rworker_get_current() == mxs_rworker_get(MXS_RWORKER_MAIN));
    mxb_assert(Worker::state() == Worker::STOPPED);
    mxb_assert(state() == MONITOR_STATE_STOPPED);
    mxb_assert(m_thread_running.load() == false);

    if (journal_is_stale())
    {
        MXS_WARNING("Removing stale journal file for monitor '%s'.", m_name);
        remove_server_journal(this);
    }

    if (!m_checked)
    {
        if (!has_sufficient_permissions())
        {
            MXS_ERROR("Failed to start monitor. See earlier errors for more information.");
        }
        else
        {
            m_checked = true;
        }
    }

    bool started = false;
    if (m_checked)
    {
        m_master = NULL;

        if (configure(pParams))
        {
            m_loop_called = get_time_ms() - m_settings.interval; // Next tick should happen immediately.
            if (!Worker::start())
            {
                MXS_ERROR("Failed to start worker for monitor '%s'.", m_name);
            }
            else
            {
                // Ok, so the thread started. Let's wait until we can be certain the
                // state has been updated.
                m_semaphore.wait();

                started = m_thread_running.load(std::memory_order_acquire);
                if (!started)
                {
                    // Ok, so the initialization failed and the thread will exit.
                    // We need to wait on it so that the thread resources will not leak.
                    Worker::join();
                }
            }
        }
    }
    return started;
}

// static
int64_t MonitorWorker::get_time_ms()
{
    timespec t;

    MXB_AT_DEBUG(int rv = ) clock_gettime(CLOCK_MONOTONIC_COARSE, &t);
    mxb_assert(rv == 0);

    return t.tv_sec * 1000 + (t.tv_nsec / 1000000);
}

bool MonitorWorker::should_update_disk_space_status(const MXS_MONITORED_SERVER* pMs) const
{
    bool should_check = false;

    if ((m_settings.disk_space_check_interval > 0)
        && (pMs->disk_space_checked != -1) // -1 means disabled
        && (!m_settings.disk_space_limits.empty() || pMs->server->have_disk_space_limits()))
    {
        int64_t now = get_time_ms();
        if (now - pMs->disk_space_checked > m_settings.disk_space_check_interval)
        {
            should_check = true;
        }
    }

    return should_check;
}

namespace
{

bool check_disk_space_exhausted(MXS_MONITORED_SERVER* pMs,
                                const std::string& path,
                                const maxscale::disk::SizesAndName& san,
                                int32_t max_percentage)
{
    bool disk_space_exhausted = false;

    int32_t used_percentage = ((san.total() - san.available()) / (double)san.total()) * 100;

    if (used_percentage >= max_percentage)
    {
        MXS_ERROR("Disk space on %s at %s is exhausted; %d%% of the the disk "
                  "mounted on the path %s has been used, and the limit it %d%%.",
                  pMs->server->name(),
                  pMs->server->address,
                  used_percentage,
                  path.c_str(),
                  max_percentage);
        disk_space_exhausted = true;
    }

    return disk_space_exhausted;
}
}

void MonitorWorker::update_disk_space_status(MXS_MONITORED_SERVER* pMs)
{
    std::map<std::string, disk::SizesAndName> info;

    int rv = disk::get_info_by_path(pMs->con, &info);

    if (rv == 0)
    {
        // Server-specific setting takes precedence.
        auto dst = pMs->server->get_disk_space_limits();
        if (dst.empty())
        {
            dst = m_settings.disk_space_limits;
        }

        bool disk_space_exhausted = false;
        int32_t star_max_percentage = -1;
        std::set<std::string> checked_paths;

        for (const auto& dst_item : dst)
        {
            string path = dst_item.first;
            int32_t max_percentage = dst_item.second;

            if (path == "*")
            {
                star_max_percentage = max_percentage;
            }
            else
            {
                auto j = info.find(path);

                if (j != info.end())
                {
                    const disk::SizesAndName& san = j->second;

                    disk_space_exhausted = check_disk_space_exhausted(pMs, path, san, max_percentage);
                    checked_paths.insert(path);
                }
                else
                {
                    MXS_WARNING("Disk space threshold specified for %s even though server %s at %s"
                                "does not have that.",
                                path.c_str(),
                                pMs->server->name(),
                                pMs->server->address);
                }
            }
        }

        if (star_max_percentage != -1)
        {
            for (auto j = info.begin(); j != info.end(); ++j)
            {
                string path = j->first;

                if (checked_paths.find(path) == checked_paths.end())
                {
                    const disk::SizesAndName& san = j->second;

                    disk_space_exhausted = check_disk_space_exhausted(pMs, path, san, star_max_percentage);
                }
            }
        }

        if (disk_space_exhausted)
        {
            pMs->pending_status |= SERVER_DISK_SPACE_EXHAUSTED;
        }
        else
        {
            pMs->pending_status &= ~SERVER_DISK_SPACE_EXHAUSTED;
        }

        pMs->disk_space_checked = get_time_ms();
    }
    else
    {
        SERVER* pServer = pMs->server;

        if (mysql_errno(pMs->con) == ER_UNKNOWN_TABLE)
        {
            // Disable disk space checking for this server.
            pMs->disk_space_checked = -1;

            MXS_ERROR("Disk space cannot be checked for %s at %s, because either the "
                      "version (%s) is too old, or the DISKS information schema plugin "
                      "has not been installed. Disk space checking has been disabled.",
                      pServer->name(),
                      pServer->address,
                      pServer->version_string().c_str());
        }
        else
        {
            MXS_ERROR("Checking the disk space for %s at %s failed due to: (%d) %s",
                      pServer->name(),
                      pServer->address,
                      mysql_errno(pMs->con),
                      mysql_error(pMs->con));
        }
    }
}

bool MonitorWorker::configure(const MXS_CONFIG_PARAMETER* pParams)
{
    return true;
}

bool MonitorWorker::has_sufficient_permissions()
{
    return true;
}

void MonitorWorker::flush_server_status()
{
    for (MXS_MONITORED_SERVER* pMs : m_servers)
    {
        if (!pMs->server->is_in_maint())
        {
            pMs->server->status = pMs->pending_status;
        }
    }
}

void MonitorWorkerSimple::pre_tick()
{
}

void MonitorWorkerSimple::post_tick()
{
}

void MonitorWorkerSimple::tick()
{
    pre_tick();

    for (MXS_MONITORED_SERVER* pMs : m_servers)
    {
        if (!pMs->server->is_in_maint())
        {
            pMs->mon_prev_status = pMs->server->status;
            pMs->pending_status = pMs->server->status;

            mxs_connect_result_t rval = pMs->ping_or_connect(m_settings.conn_settings);

            if (mon_connection_is_ok(rval))
            {
                monitor_clear_pending_status(pMs, SERVER_AUTH_ERROR);
                monitor_set_pending_status(pMs, SERVER_RUNNING);

                if (should_update_disk_space_status(pMs))
                {
                    update_disk_space_status(pMs);
                }

                update_server_status(pMs);
            }
            else
            {
                /**
                 * TODO: Move the bits that do not represent a state out of
                 * the server state bits. This would allow clearing the state by
                 * zeroing it out.
                 */
                const uint64_t bits_to_clear = ~SERVER_WAS_MASTER;

                monitor_clear_pending_status(pMs, bits_to_clear);

                if (mysql_errno(pMs->con) == ER_ACCESS_DENIED_ERROR)
                {
                    monitor_set_pending_status(pMs, SERVER_AUTH_ERROR);
                }
                else
                {
                    monitor_clear_pending_status(pMs, SERVER_AUTH_ERROR);
                }

                if (mon_status_changed(pMs) && mon_print_fail_status(pMs))
                {
                    mon_log_connect_error(pMs, rval);
                }
            }

#if defined (SS_DEBUG)
            if (mon_status_changed(pMs) || mon_print_fail_status(pMs))
            {
                // The current status is still in pMs->pending_status.
                MXS_DEBUG("Backend server [%s]:%d state : %s",
                          pMs->server->address, pMs->server->port,
                          SERVER::status_to_string(pMs->pending_status).c_str());
            }
#endif

            if (pMs->server->is_down())
            {
                pMs->mon_err_count += 1;
            }
            else
            {
                pMs->mon_err_count = 0;
            }
        }
    }

    post_tick();
}

void MonitorWorker::pre_loop()
{
}

void MonitorWorker::post_loop()
{
}

void MonitorWorker::process_state_changes()
{
    detect_handle_state_changes();
}

bool MonitorWorker::pre_run()
{
    bool rv = false;

    if (mysql_thread_init() == 0)
    {
        rv = true;
        // Write and post the semaphore to signal the admin thread that the start is succeeding.
        m_thread_running.store(true, std::memory_order_release);
        m_semaphore.post();

        load_server_journal(m_monitor, &m_master);
        pre_loop();
        delayed_call(1, &MonitorWorker::call_run_one_tick, this);
    }
    else
    {
        MXS_ERROR("mysql_thread_init() failed for %s. The monitor cannot start.", m_name);
        m_semaphore.post();
    }

    return rv;
}

void MonitorWorker::post_run()
{
    post_loop();

    mysql_thread_end();
}

bool MonitorWorker::call_run_one_tick(Worker::Call::action_t action)
{
    /** This is both the minimum sleep between two ticks and also the maximum time between early
     *  wakeup checks. */
    const int base_interval_ms = 100;
    if (action == Worker::Call::EXECUTE)
    {
        int64_t now = get_time_ms();
        // Enough time has passed,
        if ((now - m_loop_called > m_settings.interval)
            // or maintenance flag is set,
            || atomic_load_int(&this->check_status_flag) == Monitor::STATUS_FLAG_CHECK
            // or a monitor-specific condition is met.
            || immediate_tick_required())
        {
            m_loop_called = now;
            run_one_tick();
            now = get_time_ms();
        }

        int64_t ms_to_next_call = m_settings.interval - (now - m_loop_called);
        // ms_to_next_call will be negative, if the run_one_tick() call took
        // longer than one monitor interval.
        int64_t delay = ((ms_to_next_call <= 0) || (ms_to_next_call >= base_interval_ms)) ?
            base_interval_ms : ms_to_next_call;

        delayed_call(delay, &MonitorWorker::call_run_one_tick, this);
    }
    return false;
}

void MonitorWorker::run_one_tick()
{
    monitor_check_maintenance_requests(m_monitor);

    tick();
    mxb::atomic::add(&m_ticks, 1, mxb::atomic::RELAXED);

    flush_server_status();

    process_state_changes();

    mon_hangup_failed_servers(m_monitor);
    store_server_journal(m_monitor, m_master);
}

bool MonitorWorker::immediate_tick_required() const
{
    return false;
}
}

MXS_MONITORED_SERVER::MXS_MONITORED_SERVER(SERVER* server)
    : server(server)
    , disk_space_checked(maxscale::MonitorWorker::get_time_ms()) // Pretend disk space was just checked.
{
}
