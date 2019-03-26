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
 * @file blr.c - binlog router, allows MaxScale to act as an intermediatory for replication
 *
 * The binlog router is designed to be used in replication environments to
 * increase the replication fanout of a master server. It provides a transparant
 * mechanism to read the binlog entries for multiple slaves while requiring
 * only a single connection to the actual master to support the slaves.
 *
 * The current prototype implement is designed to support MySQL 5.6 and has
 * a number of limitations. This prototype is merely a proof of concept and
 * should not be considered production ready.
 */

#include "blr.hh"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <uuid/uuid.h>

#include <maxbase/atomic.h>
#include <maxbase/worker.h>
#include <maxscale/alloc.h>
#include <maxscale/config.hh>
#include <maxscale/dcb.hh>
#include <maxscale/housekeeper.h>
#include <maxscale/protocol/mysql.hh>
#include <maxscale/router.hh>
#include <maxscale/secrets.h>
#include <maxscale/server.hh>
#include <maxscale/service.hh>
#include <maxscale/users.h>
#include <maxscale/utils.h>
#include <maxscale/utils.hh>
#include <maxscale/paths.h>
#include "../../../core/internal/server.hh"

/* The router entry points */
static MXS_ROUTER*         createInstance(SERVICE* service, MXS_CONFIG_PARAMETER* params);
static void                free_instance(ROUTER_INSTANCE* instance);
static MXS_ROUTER_SESSION* newSession(MXS_ROUTER* instance,
                                      MXS_SESSION* session);
static void closeSession(MXS_ROUTER* instance,
                         MXS_ROUTER_SESSION* router_session);
static void freeSession(MXS_ROUTER* instance,
                        MXS_ROUTER_SESSION* router_session);
static int routeQuery(MXS_ROUTER* instance,
                      MXS_ROUTER_SESSION* router_session,
                      GWBUF* queue);
static void    diagnostics(MXS_ROUTER* instance, DCB* dcb);
static json_t* diagnostics_json(const MXS_ROUTER* instance);
static void    clientReply(MXS_ROUTER* instance,
                           MXS_ROUTER_SESSION* router_session,
                           GWBUF* queue,
                           DCB*   backend_dcb);
static void errorReply(MXS_ROUTER* instance,
                       MXS_ROUTER_SESSION* router_session,
                       GWBUF* message,
                       DCB*   backend_dcb,
                       mxs_error_action_t action,
                       bool* succp);

static uint64_t getCapabilities(MXS_ROUTER* instance);
static int      blr_load_dbusers(const ROUTER_INSTANCE* router);
static int      blr_check_binlog(ROUTER_INSTANCE* router);
void            blr_master_close(ROUTER_INSTANCE*);
void            blr_free_ssl_data(ROUTER_INSTANCE* inst);
static void     destroyInstance(MXS_ROUTER* instance);
bool            blr_extract_key(const char* linebuf,
                                int nline,
                                ROUTER_INSTANCE* router);
bool        blr_get_encryption_key(ROUTER_INSTANCE* router);
int         blr_parse_key_file(ROUTER_INSTANCE* router);
static bool blr_open_gtid_maps_storage(ROUTER_INSTANCE* inst);

static bool stats_func(void*);

static bool rses_begin_locked_router_action(ROUTER_SLAVE*);
static void rses_end_locked_router_action(ROUTER_SLAVE*);
GWBUF*      blr_cache_read_response(ROUTER_INSTANCE* router,
                                    char* response);
extern bool blr_load_last_mariadb_gtid(ROUTER_INSTANCE* router,
                                       MARIADB_GTID_INFO* result);

static pthread_mutex_t instlock;
static ROUTER_INSTANCE* instances;

static const MXS_ENUM_VALUE enc_algo_values[] =
{
    {"aes_cbc", BLR_AES_CBC},
#if OPENSSL_VERSION_NUMBER > 0x10000000L
    {"aes_ctr", BLR_AES_CTR},
#endif
    {NULL}
};

static const MXS_ENUM_VALUE binlog_storage_values[] =
{
    {"flat", BLR_BINLOG_STORAGE_FLAT},
    {"tree", BLR_BINLOG_STORAGE_TREE},
    {NULL}
};

/**
 * The module entry point routine. It is this routine that
 * must populate the structure that is referred to as the
 * "module object", this is a structure with the set of
 * external entry points for this module.
 *
 * @return The module object
 */
extern "C" MXS_MODULE* MXS_CREATE_MODULE()
{
    MXS_NOTICE("Initialise binlog router module.");
    pthread_mutex_init(&instlock, NULL);
    instances = NULL;

    static MXS_ROUTER_OBJECT MyObject =
    {
        createInstance,
        newSession,
        closeSession,
        freeSession,
        routeQuery,
        diagnostics,
        diagnostics_json,
        clientReply,
        errorReply,
        getCapabilities,
        destroyInstance
    };

    static MXS_MODULE info =
    {
        MXS_MODULE_API_ROUTER,
        MXS_MODULE_GA,
        MXS_ROUTER_VERSION,
        "Binlogrouter",
        "V2.1.0",
        RCAP_TYPE_NO_RSESSION | RCAP_TYPE_CONTIGUOUS_OUTPUT
        | RCAP_TYPE_RESULTSET_OUTPUT | RCAP_TYPE_NO_AUTH,
        &MyObject,
        NULL,                                           /* Process init. */
        NULL,                                           /* Process finish. */
        NULL,                                           /* Thread init. */
        NULL,                                           /* Thread finish. */
        {
            {"uuid",
             MXS_MODULE_PARAM_STRING},
            {"server_id",
             MXS_MODULE_PARAM_COUNT},
            {"master_id",                                MXS_MODULE_PARAM_COUNT,
             "0"},
            {"master_uuid",
             MXS_MODULE_PARAM_STRING},
            {"master_version",
             MXS_MODULE_PARAM_STRING},
            {"master_hostname",
             MXS_MODULE_PARAM_STRING},
            {"slave_hostname",
             MXS_MODULE_PARAM_STRING},
            {"mariadb10-compatibility",                  MXS_MODULE_PARAM_BOOL,
             "true"},
            {"maxwell-compatibility",                    MXS_MODULE_PARAM_BOOL,
             "false"},
            {"filestem",                                 MXS_MODULE_PARAM_STRING,
             BINLOG_NAME_ROOT},
            {"file",                                     MXS_MODULE_PARAM_COUNT,
             "1"},
            {"transaction_safety",                       MXS_MODULE_PARAM_BOOL,
             "false"},
            {"semisync",                                 MXS_MODULE_PARAM_BOOL,
             "false"},
            {"encrypt_binlog",                           MXS_MODULE_PARAM_BOOL,
             "false"},
            {
                "encryption_algorithm",                  MXS_MODULE_PARAM_ENUM,
                "aes_cbc",
                MXS_MODULE_OPT_NONE,                     enc_algo_values
            },
            {"encryption_key_file",                      MXS_MODULE_PARAM_PATH,
             NULL,
             MXS_MODULE_OPT_PATH_R_OK},
            {"mariadb10_master_gtid",                    MXS_MODULE_PARAM_BOOL,
             "false"},
            {
                "binlog_structure",                      MXS_MODULE_PARAM_ENUM,
                "flat",
                MXS_MODULE_OPT_NONE,                     binlog_storage_values
            },
            {"shortburst",                               MXS_MODULE_PARAM_COUNT,
             DEF_SHORT_BURST},
            {"longburst",                                MXS_MODULE_PARAM_COUNT,
             DEF_LONG_BURST},
            {"burstsize",                                MXS_MODULE_PARAM_SIZE,
             DEF_BURST_SIZE},
            {"heartbeat",                                MXS_MODULE_PARAM_COUNT,
             BLR_HEARTBEAT_DEFAULT_INTERVAL},
            {"connect_retry",                            MXS_MODULE_PARAM_COUNT,
             BLR_MASTER_CONNECT_RETRY},
            {"master_retry_count",                       MXS_MODULE_PARAM_COUNT,
             BLR_MASTER_RETRY_COUNT},
            {"send_slave_heartbeat",                     MXS_MODULE_PARAM_BOOL,
             "false"},
            {
                "binlogdir",
                MXS_MODULE_PARAM_PATH,
                MXS_DEFAULT_DATADIR,
                MXS_MODULE_OPT_PATH_R_OK
                | MXS_MODULE_OPT_PATH_W_OK
                | MXS_MODULE_OPT_PATH_X_OK
                | MXS_MODULE_OPT_PATH_CREAT
            },
            {"ssl_cert_verification_depth",              MXS_MODULE_PARAM_COUNT,
             "9"},
            {MXS_END_MODULE_PARAMS}
        }
    };

    return &info;
}

/**
 * Create an instance of the router for a particular service
 * within MaxScale.
 *
 * The process of creating the instance causes the router to register
 * with the master server and begin replication of the binlogs from
 * the master server to MaxScale.
 *
 * @param service   The service this router is being create for
 * @param options   An array of options for this query router
 *
 * @return The instance data for this new instance
 */
static MXS_ROUTER* createInstance(SERVICE* service, MXS_CONFIG_PARAMETER* params)
{
    ROUTER_INSTANCE* inst;
    uuid_t defuuid;
    int rc = 0;
    char task_name[BLRM_TASK_NAME_LEN + 1] = "";
    const char* user;
    const char* password;
    serviceGetUser(service, &user, &password);

    if (!user[0] || !password[0])
    {
        MXS_ERROR("%s: Error: Service is missing user credentials."
                  " Add the missing username or passwd parameter to the service.",
                  service->name());
        return NULL;
    }

    /*
     * We only support one server behind this router, since the server is
     * the master from which we replicate binlog records. Therefore check
     * that only one server has been defined.
     */
    if (service->dbref != NULL)
    {
        MXS_WARNING("%s: backend database server is provided by master.ini file "
                    "for use with the binlog router."
                    " Server section is no longer required.",
                    service->name());

        MXS_FREE(service->dbref);
        service->dbref = NULL;
    }

    if ((inst = (ROUTER_INSTANCE*)MXS_CALLOC(1, sizeof(ROUTER_INSTANCE))) == NULL)
    {
        return NULL;
    }

    memset(&inst->stats, 0, sizeof(ROUTER_STATS));
    memset(&inst->saved_master, 0, sizeof(MASTER_RESPONSES));

    inst->service = service;
    pthread_mutex_init(&inst->lock, NULL);
    inst->files = NULL;
    pthread_mutex_init(&inst->fileslock, NULL);
    pthread_mutex_init(&inst->binlog_lock, NULL);

    inst->binlog_fd = -1;
    inst->master_chksum = true;

    inst->master_state = BLRM_UNCONFIGURED;
    inst->master = NULL;
    inst->client = NULL;

    inst->user = MXS_STRDUP_A(user);
    inst->password = MXS_STRDUP_A(password);
    inst->retry_count = 0;
    inst->m_errno = 0;
    inst->m_errmsg = NULL;

    memset(&inst->pending_transaction, '\0', sizeof(PENDING_TRANSACTION));
    inst->last_safe_pos = 0;
    inst->last_event_pos = 0;

    /* SSL replication is disabled by default */
    inst->ssl_enabled = 0;
    /* SSL config options */
    inst->ssl_ca = NULL;
    inst->ssl_cert = NULL;
    inst->ssl_key = NULL;
    inst->ssl_version = NULL;

    inst->active_logs = 0;
    inst->reconnect_pending = 0;
    inst->handling_threads = 0;
    inst->rotating = 0;
    inst->slaves = NULL;
    inst->next = NULL;
    inst->lastEventTimestamp = 0;
    inst->binlog_position = 0;
    inst->current_pos = 0;
    inst->current_safe_event = 0;
    inst->master_event_state = BLR_EVENT_DONE;
    inst->last_mariadb_gtid[0] = '\0';

    strcpy(inst->binlog_name, "");
    strcpy(inst->prevbinlog, "");

    inst->initbinlog = params->get_integer("file");

    inst->short_burst = params->get_integer("shortburst");
    inst->long_burst = params->get_integer("longburst");
    inst->burst_size = params->get_size("burstsize");
    inst->binlogdir = params->get_c_str_copy("binlogdir");
    inst->heartbeat = params->get_integer("heartbeat");
    inst->retry_interval = params->get_integer("connect_retry");
    inst->retry_limit = params->get_integer("master_retry_count");
    inst->ssl_cert_verification_depth = params->get_integer("ssl_cert_verification_depth");
    inst->mariadb10_compat = params->get_bool("mariadb10-compatibility");
    inst->maxwell_compat = params->get_bool("maxwell-compatibility");
    inst->trx_safe = params->get_bool("transaction_safety");
    inst->fileroot = params->get_c_str_copy("filestem");
    inst->heartbeat_task_active = false;

    /* Server id */
    inst->serverid = params->get_integer("server_id");

    /* Identity options */
    inst->set_master_version = params->get_c_str_copy("master_version");
    inst->set_master_hostname = params->get_c_str_copy("master_hostname");
    inst->set_slave_hostname = params->get_c_str_copy("slave_hostname");
    inst->masterid = params->get_integer("master_id");
    inst->set_master_server_id = inst->masterid != 0;
    inst->master_uuid = params->get_c_str_copy("master_uuid");
    inst->set_master_uuid = inst->master_uuid != NULL;

    /* Slave Heartbeat */
    inst->send_slave_heartbeat = params->get_bool("send_slave_heartbeat");

    /* Semi-Sync support */
    inst->request_semi_sync = params->get_bool("semisync");
    inst->master_semi_sync = 0;

    /* Enable MariaDB GTID tracking for slaves if MariaDB 10 compat is set */
    inst->mariadb10_gtid = inst->mariadb10_compat;

    /* Enable MariaDB GTID registration to master */
    inst->mariadb10_master_gtid = params->get_bool("mariadb10_master_gtid");

    /* Binlog encryption */
    inst->encryption.enabled = params->get_bool("encrypt_binlog");
    inst->encryption.encryption_algorithm = params->get_enum("encryption_algorithm", enc_algo_values);
    inst->encryption.key_management_filename = params->get_c_str_copy("encryption_key_file");

    /* Encryption CTX */
    inst->encryption_ctx = NULL;

    /* Set router uuid */
    inst->uuid = params->get_c_str_copy("uuid");

    /* Set Flat storage of binlog files as default */
    inst->storage_type = BLR_BINLOG_STORAGE_FLAT;

    if (inst->uuid == NULL)
    {
        /* Generate UUID for the router instance */
        uuid_generate_time(defuuid);

        if ((inst->uuid = (char*)MXS_CALLOC(38, 1)) != NULL)
        {
            sprintf(inst->uuid,
                    "%02hhx%02hhx%02hhx%02hhx-"
                    "%02hhx%02hhx-"
                    "%02hhx%02hhx-"
                    "%02hhx%02hhx-"
                    "%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
                    defuuid[0],
                    defuuid[1],
                    defuuid[2],
                    defuuid[3],
                    defuuid[4],
                    defuuid[5],
                    defuuid[6],
                    defuuid[7],
                    defuuid[8],
                    defuuid[9],
                    defuuid[10],
                    defuuid[11],
                    defuuid[12],
                    defuuid[13],
                    defuuid[14],
                    defuuid[15]);
        }
        else
        {
            free_instance(inst);
            return NULL;
        }
    }

    std::vector<std::string> options = mxs::strtok(params->get_string("router_options"), ", \t");

    /*
     * Process the options.
     * We have an array of attribute values passed to us that we must
     * examine. Supported attributes are:
     *  uuid=
     *  server-id=
     *  user=
     *  password=
     *  master-id=
     *  filestem=
     */

    for (auto&& p : options)
    {
        auto kv = mxs::strtok(p, "=");

        if (kv.size() != 2)
        {
            MXS_ERROR("Unsupported router option %s for binlog router.", p.c_str());
        }
        else
        {
            auto& k = kv[0];
            auto& v = kv[1];

            if (k == "uuid")
            {
                MXS_FREE(inst->uuid);
                inst->uuid = MXS_STRDUP_A(v.c_str());
            }
            else if (k == "server_id")
            {
                if ((inst->serverid = atoi(v.c_str())) <= 0)
                {
                    MXS_ERROR("Service %s, invalid server-id '%s'. "
                              "Please configure it with a unique positive integer value (1..2^32-1)",
                              service->name(),
                              v.c_str());
                    free_instance(inst);
                    return NULL;
                }
            }
            else if (k == "user")
            {
                MXS_FREE(inst->user);
                inst->user = MXS_STRDUP_A(v.c_str());
            }
            else if (k == "password")
            {
                MXS_FREE(inst->password);
                inst->password = MXS_STRDUP_A(v.c_str());
            }
            else if (k == "passwd")
            {
                MXS_FREE(inst->password);
                inst->password = MXS_STRDUP_A(v.c_str());
            }
            else if (k == "master_id")
            {
                int master_id = atoi(v.c_str());
                if (master_id > 0)
                {
                    inst->masterid = master_id;
                    inst->set_master_server_id = true;
                }
            }
            else if (k == "master_uuid")
            {
                inst->set_master_uuid = true;
                MXS_FREE(inst->master_uuid);
                inst->master_uuid = MXS_STRDUP_A(v.c_str());
            }
            else if (k == "master_version")
            {
                MXS_FREE(inst->set_master_version);
                inst->set_master_version = MXS_STRDUP_A(v.c_str());
            }
            else if (k == "master_hostname")
            {
                MXS_FREE(inst->set_master_hostname);
                inst->set_master_hostname = MXS_STRDUP_A(v.c_str());
            }
            else if (k == "slave_hostname")
            {
                MXS_FREE(inst->set_slave_hostname);
                inst->set_slave_hostname = MXS_STRDUP_A(v.c_str());
            }
            else if (k == "mariadb10-compatibility")
            {
                inst->mariadb10_compat = config_truth_value(v.c_str());
            }
            else if (k == "maxwell-compatibility")
            {
                inst->maxwell_compat = config_truth_value(v.c_str());
            }
            else if (k == "filestem")
            {
                MXS_FREE(inst->fileroot);
                inst->fileroot = MXS_STRDUP_A(v.c_str());
            }
            else if (k == "file")
            {
                inst->initbinlog = atoi(v.c_str());
            }
            else if (k == "transaction_safety")
            {
                inst->trx_safe = config_truth_value(v.c_str());
            }
            else if (k == "semisync")
            {
                inst->request_semi_sync = config_truth_value(v.c_str());
            }
            else if (k == "encrypt_binlog")
            {
                inst->encryption.enabled = config_truth_value(v.c_str());
            }
            else if (k == "mariadb10_master_gtid")
            {
                inst->mariadb10_master_gtid = config_truth_value(v.c_str());
            }
            else if (k == "encryption_algorithm")
            {
                int ret = blr_check_encryption_algorithm(v.c_str());
                if (ret > -1)
                {
                    inst->encryption.encryption_algorithm = ret;
                }
                else
                {
                    MXS_ERROR("Service %s, invalid encryption_algorithm '%s'. "
                              "Supported algorithms: %s",
                              service->name(),
                              v.c_str(),
                              blr_encryption_algorithm_list());
                    free_instance(inst);
                    return NULL;
                }
            }
            else if (k == "encryption_key_file")
            {
                MXS_FREE(inst->encryption.key_management_filename);
                inst->encryption.key_management_filename = MXS_STRDUP_A(v.c_str());
            }
            else if (k == "shortburst")
            {
                inst->short_burst = atoi(v.c_str());
            }
            else if (k == "longburst")
            {
                inst->long_burst = atoi(v.c_str());
            }
            else if (k == "burstsize")
            {
                unsigned long size = atoi(v.c_str());
                const char* ptr = v.c_str();
                while (*ptr && isdigit(*ptr))
                {
                    ptr++;
                }
                switch (*ptr)
                {
                case 'G':
                case 'g':
                    size = size * 1024 * 1000 * 1000;
                    break;

                case 'M':
                case 'm':
                    size = size * 1024 * 1000;
                    break;

                case 'K':
                case 'k':
                    size = size * 1024;
                    break;
                }
                inst->burst_size = size;
            }
            else if (k == "heartbeat")
            {
                int h_val = (int)strtol(v.c_str(), NULL, 10);

                if (h_val < 0 || (errno == ERANGE) || h_val > BLR_HEARTBEAT_MAX_INTERVAL)
                {
                    MXS_WARNING("Invalid heartbeat period %s. Setting it to default value %ld.",
                                v.c_str(),
                                inst->heartbeat);
                }
                else
                {
                    inst->heartbeat = h_val;
                }
            }
            else if (k == "connect_retry")
            {
                inst->retry_interval = atoi(v.c_str());
            }
            else if (k == "master_retry_count")
            {
                inst->retry_limit = atoi(v.c_str());
            }
            else if (k == "send_slave_heartbeat")
            {
                inst->send_slave_heartbeat = config_truth_value(v.c_str());
            }
            else if (k == "binlogdir")
            {
                MXS_FREE(inst->binlogdir);
                inst->binlogdir = MXS_STRDUP_A(v.c_str());
            }
            else if (k == "ssl_cert_verification_depth")
            {
                if ((inst->ssl_cert_verification_depth = atoi(v.c_str())) < 0)
                {
                    MXS_ERROR("%s: invalid Master ssl_cert_verification_depth %s."
                              " Setting it to default value %i.",
                              service->name(),
                              v.c_str(),
                              inst->ssl_cert_verification_depth);
                    free_instance(inst);
                    return NULL;
                }
            }
            else
            {
                MXS_WARNING("%s: unsupported router option %s for binlog router.",
                            service->name(),
                            k.c_str());
            }
        }
    }

    inst->orig_masterid = 0;
    inst->mariadb10_gtid_domain = BLR_DEFAULT_GTID_DOMAIN_ID;

    /* Override master_id */
    if (inst->masterid)
    {
        inst->set_master_server_id = true;
    }

    /* Check master connect options */
    if (inst->heartbeat < 0)
    {
        MXS_ERROR("%s: invalid 'heartbeat' value.",
                  service->name());
        free_instance(inst);
        return NULL;
    }

    if (inst->heartbeat == 0)
    {
        blr_log_disabled_heartbeat(inst);
    }

    if (inst->retry_interval <= 0)
    {
        MXS_ERROR("%s: invalid 'connect_retry' value.",
                  service->name());
        free_instance(inst);
        return NULL;
    }

    if (inst->retry_limit <= 0)
    {
        MXS_ERROR("%s: invalid 'master_retry_count' value.",
                  service->name());
        free_instance(inst);
        return NULL;
    }

    /* Check BinlogDir option */
    if ((inst->binlogdir == NULL)
        || (inst->binlogdir != NULL
            && !strlen(inst->binlogdir)))
    {
        MXS_ERROR("Service %s, binlog directory is not specified",
                  service->name());
        free_instance(inst);
        return NULL;
    }

    if (inst->serverid <= 0)
    {
        MXS_ERROR("Service %s, server_id is not configured. "
                  "Please configure it with a unique positive "
                  "integer value (1..2^32-1)",
                  service->name());
        free_instance(inst);
        return NULL;
    }

    /* Get the Encryption key */
    if (inst->encryption.enabled && !blr_get_encryption_key(inst))
    {
        free_instance(inst);
        return NULL;
    }

    /**
     * If binlogdir is not found create it
     * On failure don't start the instance
     */
    if (access(inst->binlogdir, R_OK) == -1)
    {
        int mkdir_rval;
        mkdir_rval = mkdir(inst->binlogdir, 0700);
        if (mkdir_rval == -1)
        {
            MXS_ERROR("Service %s, Failed to create binlog directory '%s': [%d] %s",
                      service->name(),
                      inst->binlogdir,
                      errno,
                      mxs_strerror(errno));

            free_instance(inst);
            return NULL;
        }
    }

    /**
     * Check mariadb10_compat option before any other mariadb10 option.
     */
    if (!inst->mariadb10_compat
        && inst->mariadb10_master_gtid)
    {
        MXS_WARNING("MariaDB Master GTID registration needs"
                    " MariaDB compatibilty option. The 'mariadb10-compatibility'"
                    " has been turned on. Please permanently enable it with option"
                    " 'mariadb10-compatibility=On'");

        inst->mariadb10_compat = true;
    }

    if (inst->mariadb10_master_gtid)
    {
        /* Force GTID slave request handling */
        inst->mariadb10_gtid = true;
        /* Force transaction safety */
        inst->trx_safe = true;
        /* Force binlog storage as tree */
        inst->storage_type = BLR_BINLOG_STORAGE_TREE;
    }

    /* Log binlog structure storage mode */
    MXS_NOTICE("%s: storing binlog files in %s",
               service->name(),
               inst->storage_type == BLR_BINLOG_STORAGE_FLAT ?
               "'flat' mode" :
               "'tree' mode using GTID domain_id and server_id");

    /* Enable MariaDB the GTID maps store */
    if (inst->mariadb10_compat)
    {
        /* Create/Open R/W GTID sqlite3 storage */
        if (!blr_open_gtid_maps_storage(inst))
        {
            free_instance(inst);
            return NULL;
        }
    }

    /* Dynamically allocate master_host server struct, not written in any cnf file */
    if (service->dbref == NULL)
    {
        // Declared in config.cc and needs to be removed if/when blr is refactored
        extern const MXS_MODULE_PARAM config_server_params[];
        MXS_CONFIG_PARAMETER params;
        params.set_from_list({
                                {"address", "_none_"},
                                {"port", "3306"},
                                {"protocol", "mariadbbackend"},
                                {"authenticator", "MySQLBackendAuth"}
                             }, config_server_params);

        Server* server = Server::server_alloc("binlog_router_master_host", &params);

        if (server == NULL)
        {
            MXS_ERROR("%s: Error for server_alloc in createInstance",
                      inst->service->name());

            sqlite3_close_v2(inst->gtid_maps);
            free_instance(inst);
            return NULL;
        }

        SSL_LISTENER* ssl_cfg;
        /* Allocate SSL struct for backend connection */
        if ((ssl_cfg =
                 static_cast<SSL_LISTENER*>(MXS_CALLOC(1, sizeof(SSL_LISTENER)))) == NULL)
        {
            MXS_ERROR("%s: Error allocating memory for SSL struct in createInstance",
                      inst->service->name());

            MXS_FREE(service->dbref);
            sqlite3_close_v2(inst->gtid_maps);
            free_instance(inst);
            return NULL;
        }

        /* Set some SSL defaults */
        ssl_cfg->ssl_init_done = false;
        ssl_cfg->ssl_method_type = SERVICE_SSL_TLS_MAX;
        ssl_cfg->ssl_cert_verify_depth = 9;
        ssl_cfg->ssl_verify_peer_certificate = true;

        /** Set SSL pointer in in server struct */
        server->server_ssl = ssl_cfg;

        /* Add server to service backend list */
        serviceAddBackend(inst->service, server);

        /* Hide backend server struct */
        service->dbref->server->is_active = false;
        service->dbref->active = false;
    }

    /*
     * Check for master.ini file with master connection details
     * If not found a CHANGE MASTER TO is required via mysqsl client.
     * Use START SLAVE for replication startup.
     *
     * If existent master.ini will be used for
     * automatic master replication start phase
     */

    inst->config_index = 0;
    rc = blr_file_read_master_config(inst);

    /*
     * retcode:
     * -1 file not found, 0 parsing ok, > 0 error parsing the content
     */

    if (rc != 0)
    {
        if (rc == -1)
        {
            MXS_WARNING("%s: master.ini file not found in %s."
                        " Master registration cannot be started."
                        " Configure with CHANGE MASTER TO ...",
                        inst->service->name(),
                        inst->binlogdir);
        }
        else
        {
            MXS_ERROR("%s: master.ini file with errors in %s."
                      " Master registration cannot be started."
                      " Fix errors in it or configure with CHANGE MASTER TO ...",
                      inst->service->name(),
                      inst->binlogdir);
        }
    }
    else
    {
        inst->master_state = BLRM_UNCONNECTED;
        /* Set backend server as active */
        service->dbref->server->is_active = true;
        service->dbref->active = true;
    }

    /**
     *******************************
     * Initialise the binlog router
     *******************************
     */

    /**
     * Check first for SSL enabled replication.
     * If not remove the SSL struct from server
     */

    if (inst->ssl_enabled)
    {
        MXS_INFO("%s: Replicating from master with SSL", service->name());
    }
    else
    {
        MXS_DEBUG("%s: Replicating from master without SSL", service->name());
        /* Free the SSL struct because is not needed if MASTER_SSL = 0
         * Provided options, if any, are kept in inst->ssl_* vars
         * SHOW SLAVE STATUS can display those values
         */

        /* Note: SSL struct in server should be freed by server_free() */
        if (service->dbref && service->dbref->server)
        {
            blr_free_ssl_data(inst);
        }
    }

    if (inst->master_state == BLRM_UNCONNECTED)
    {

        /* Read any cached response messages */
        blr_cache_read_master_data(inst);

        /**
         * The value of master checksum is known only at registration time, so
         * as soon as replication succeds the value is updated.
         * Set now the binlog checksum from the saved value.
         * This is very useful in case of possible failure in the
         * registration phase for any reason: master is down, wrong password etc.
         * In this case a connecting slave will get the checksum value
         * from previous registration instead of default one (CRC32)
         * which can be wrong if slave has binlog_checksum = NONE.
         */
        blr_set_checksum(inst, inst->saved_master.chksum2);

        /*
         * Find latest binlog file in binlogdir or GTID maps repo
         */
        if (blr_file_init(inst) == 0)
        {
            MXS_ERROR("%s: Service not started due to lack of binlog directory %s",
                      service->name(),
                      inst->binlogdir);

            if (service->dbref && service->dbref->server)
            {
                /* Free SSL data */
                blr_free_ssl_data(inst);
                MXS_FREE(service->dbref);
                service->dbref = NULL;
            }

            sqlite3_close_v2(inst->gtid_maps);
            free_instance(inst);
            return NULL;
        }
    }

    /**
     * We have completed the creation of the instance data, so now
     * insert this router instance into the linked list of routers
     * that have been created with this module.
     */
    pthread_mutex_lock(&instlock);
    inst->next = instances;
    instances = inst;
    pthread_mutex_unlock(&instlock);

    /*
     * Initialise the binlog cache for this router instance
     */
    blr_init_cache(inst);

    /*
     * Add tasks for statistic computation
     */
    snprintf(task_name, BLRM_TASK_NAME_LEN, "%s stats", service->name());
    hktask_add(task_name, stats_func, inst, BLR_STATS_FREQ);

    /* Log whether the transaction safety option value is on */
    if (inst->trx_safe)
    {
        MXS_NOTICE("%s: Service has transaction safety option set to ON",
                   service->name());
    }

    /* Log whether the binlog encryption option value is on */
    if (inst->encryption.enabled)
    {
        MXS_NOTICE("%s: Service has binlog encryption set to ON, algorithm: %s,"
                   " KEY len %lu bits",
                   service->name(),
                   blr_get_encryption_algorithm(inst->encryption.encryption_algorithm),
                   8 * inst->encryption.key_len);
    }

    /**
     * Check whether replication can be started
     */
    if (inst->master_state == BLRM_UNCONNECTED)
    {
        char f_prefix[BINLOG_FILE_EXTRA_INFO] = "";
        if (inst->storage_type == BLR_BINLOG_STORAGE_TREE)
        {
            sprintf(f_prefix,
                    "%" PRIu32 "/%" PRIu32 "/",
                    inst->mariadb10_gtid_domain,
                    inst->orig_masterid);
        }

        /* Log current binlog, possibly with tree prefix */
        MXS_NOTICE("Validating last binlog file '%s%s' ...",
                   f_prefix,
                   inst->binlog_name);

        /* Check current binlog */
        if (!blr_check_binlog(inst))
        {
            if (inst->trx_safe || inst->encryption.enabled)
            {
                MXS_ERROR("The replication from master cannot be started"
                          " due to errors in current binlog file");
                /* Don't start replication, just return */
                return (MXS_ROUTER*)inst;
            }
        }

        /* Log current pos in binlog file and last seen transaction pos */
        MXS_INFO("Current binlog file is %s, safe pos %lu, current pos is %lu",
                 inst->binlog_name,
                 inst->binlog_position,
                 inst->current_pos);

        /**
         *  Try loading last found GTID if the file size is <= 4 bytes
         */
        if (inst->mariadb10_master_gtid
            && inst->current_pos <= 4)
        {
            MARIADB_GTID_INFO last_gtid;
            memset(&last_gtid, 0, sizeof(last_gtid));

            /* Get last MariaDB GTID from repo */
            if (blr_load_last_mariadb_gtid(inst, &last_gtid)
                && last_gtid.gtid[0])
            {
                /* Set MariaDB GTID */
                strcpy(inst->last_mariadb_gtid, last_gtid.gtid);
            }
            else
            {
                /**
                 * In case of no GTID, inst->last_mariadb_gtid is empty.
                 *
                 * If connecting to master with GTID = "" the server
                 * will send data from its first binlog and
                 * this might overwrite existing data.
                 *
                 * Binlog server will not connect to master.
                 *
                 * It's needed to connect to MySQL admin interface
                 * and explicitely issue:
                 * SET @@GLOBAL.GTID_SLAVE_POS =''
                 * and START SLAVE
                 */

                /* Force STOPPED state */
                inst->master_state = BLRM_SLAVE_STOPPED;
                /* Set current binlog file to empy value */
                *inst->binlog_name = 0;
                /* Set mysql_errno and error message */
                inst->m_errno = BINLOG_FATAL_ERROR_READING;
                inst->m_errmsg = MXS_STRDUP_A("HY000 Cannot find any GTID"
                                              " in the GTID maps repo."
                                              " Please issue SET @@GLOBAL.GTID_SLAVE_POS =''"
                                              " and START SLAVE."
                                              " Existing binlogs might be overwritten.");
                MXS_ERROR("%s: %s",
                          inst->service->name(),
                          inst->m_errmsg);

                return (MXS_ROUTER*)inst;
            }
        }

        /**
         *  Don't start replication if binlog has MXS_START_ENCRYPTION_EVENT
         *  but binlog encryption is off
         */
        if (!inst->encryption.enabled && inst->encryption_ctx)
        {
            MXS_ERROR("Found MXS_START_ENCRYPTION_EVENT but "
                      "binlog encryption option is currently Off. Replication can't start right now. "
                      "Please restart MaxScale with option set to On");

            /* Force STOPPED state */
            inst->master_state = BLRM_SLAVE_STOPPED;
            /* Set mysql_errno and error message */
            inst->m_errno = BINLOG_FATAL_ERROR_READING;
            inst->m_errmsg = MXS_STRDUP_A("HY000 Binlog encryption is Off"
                                          " but current binlog file has"
                                          " the MXS_START_ENCRYPTION_EVENT");

            return (MXS_ROUTER*)inst;
        }

        /* Start replication from master server */
        blr_start_master_in_main(inst);
    }

    return (MXS_ROUTER*)inst;
}

/**
 * Free the router instance
 *
 * @param    instance    The router instance
 */
static void free_instance(ROUTER_INSTANCE* instance)
{
    MXS_FREE(instance->uuid);
    MXS_FREE(instance->user);
    MXS_FREE(instance->password);
    MXS_FREE(instance->set_master_version);
    MXS_FREE(instance->set_master_hostname);
    MXS_FREE(instance->set_slave_hostname);
    MXS_FREE(instance->fileroot);
    MXS_FREE(instance->binlogdir);
    /* SSL options */
    MXS_FREE(instance->ssl_ca);
    MXS_FREE(instance->ssl_cert);
    MXS_FREE(instance->ssl_key);
    MXS_FREE(instance->ssl_version);

    MXS_FREE(instance);
}

/**
 * Associate a new session with this instance of the router.
 *
 * In the case of the binlog router a new session equates to a new slave
 * connecting to MaxScale and requesting binlog records. We need to go
 * through the slave registration process for this new slave.
 *
 * @param instance  The router instance data
 * @param session   The session itself
 * @return Session specific data for this session
 */
static MXS_ROUTER_SESSION* newSession(MXS_ROUTER* instance, MXS_SESSION* session)
{
    ROUTER_INSTANCE* inst = (ROUTER_INSTANCE*)instance;
    ROUTER_SLAVE* slave;

    MXS_DEBUG("binlog router: %lu [newSession] new router session with "
              "session %p, and inst %p.",
              pthread_self(),
              session,
              inst);

    if ((slave = (ROUTER_SLAVE*)MXS_CALLOC(1, sizeof(ROUTER_SLAVE))) == NULL)
    {
        return NULL;
    }

    memset(&slave->stats, 0, sizeof(SLAVE_STATS));
    atomic_add(&inst->stats.n_slaves, 1);
    slave->state = BLRS_CREATED;        /* Set initial state of the slave */
    slave->cstate = 0;
    slave->pthread = 0;
    slave->overrun = 0;
    slave->uuid = NULL;
    slave->hostname = NULL;
    pthread_mutex_init(&slave->catch_lock, NULL);
    slave->dcb = session->client_dcb;
    slave->router = inst;
#ifdef BLFILE_IN_SLAVE
    slave->file = NULL;
#endif
    strcpy(slave->binlog_name, "unassigned");
    slave->connect_time = time(0);
    slave->lastEventTimestamp = 0;
    slave->mariadb10_compat = false;
    slave->heartbeat = 0;
    slave->lastEventReceived = 0;
    slave->encryption_ctx = NULL;
    slave->mariadb_gtid = NULL;
    slave->gtid_maps = NULL;
    memset(&slave->f_info, 0, sizeof(MARIADB_GTID_INFO));
    slave->annotate_rows = false;
    slave->warning_msg = NULL;

    /**
     * Add this session to the list of active sessions.
     */
    pthread_mutex_lock(&inst->lock);
    slave->next = inst->slaves;
    inst->slaves = slave;
    pthread_mutex_unlock(&inst->lock);

    return reinterpret_cast<MXS_ROUTER_SESSION*>(slave);
}

/**
 * The session is no longer required. Shutdown all operation and free memory
 * associated with this session. In this case a single session is associated
 * to a slave of MaxScale. Therefore this is called when that slave is no
 * longer active and should remove of reference to that slave, free memory
 * and prevent any further forwarding of binlog records to that slave.
 *
 * Parameters:
 * @param router_instance   The instance of the router
 * @param router_cli_ses    The particular session to free
 *
 */
static void freeSession(MXS_ROUTER* router_instance,
                        MXS_ROUTER_SESSION* router_client_ses)
{
    ROUTER_INSTANCE* router = (ROUTER_INSTANCE*)router_instance;
    ROUTER_SLAVE* slave = (ROUTER_SLAVE*)router_client_ses;

    MXB_AT_DEBUG(int prev_val = ) atomic_add(&router->stats.n_slaves, -1);
    mxb_assert(prev_val > 0);

    /*
     * Remove the slave session form the list of slaves that are using the
     * router currently.
     */
    pthread_mutex_lock(&router->lock);
    if (router->slaves == slave)
    {
        router->slaves = slave->next;
    }
    else
    {
        ROUTER_SLAVE* ptr = router->slaves;

        while (ptr != NULL && ptr->next != slave)
        {
            ptr = ptr->next;
        }

        if (ptr != NULL)
        {
            ptr->next = slave->next;
        }
    }
    pthread_mutex_unlock(&router->lock);

    MXS_DEBUG("%lu [freeSession] Unlinked router_client_session %p from "
              "router %p. Connections : %d. ",
              pthread_self(),
              slave,
              router,
              prev_val - 1);

    if (slave->hostname)
    {
        MXS_FREE(slave->hostname);
    }
    if (slave->user)
    {
        MXS_FREE(slave->user);
    }
    if (slave->passwd)
    {
        MXS_FREE(slave->passwd);
    }
    if (slave->encryption_ctx)
    {
        MXS_FREE(slave->encryption_ctx);
    }
    if (slave->mariadb_gtid)
    {
        MXS_FREE(slave->mariadb_gtid);
    }
    MXS_FREE(slave->warning_msg);
    MXS_FREE(slave);
}

/**
 * Close a session with the router, this is the mechanism
 * by which a router may cleanup data structure etc.
 *
 * @param instance      The router instance data
 * @param router_session    The session being closed
 */
static void closeSession(MXS_ROUTER* instance, MXS_ROUTER_SESSION* router_session)
{
    ROUTER_INSTANCE* router = (ROUTER_INSTANCE*)instance;
    ROUTER_SLAVE* slave = (ROUTER_SLAVE*)router_session;

    if (slave == NULL)
    {
        /*
         * We must be closing the master session.
         */
        MXS_NOTICE("%s: Master %s disconnected after %ld seconds. "
                   "%lu events read,",
                   router->service->name(),
                   router->service->dbref->server->address,
                   time(0) - router->connect_time,
                   router->stats.n_binlogs_ses);
        MXS_ERROR("Binlog router close session with master server %s",
                  router->service->dbref->server->name());
        blr_master_reconnect(router);
        return;
    }

    /**
     * Lock router client session for secure read and update.
     */
    if (rses_begin_locked_router_action(slave))
    {
        /* decrease server registered slaves counter */
        atomic_add(&router->stats.n_registered, -1);

        if (slave->state > 0)
        {
            MXS_NOTICE("%s: Slave [%s]:%d, server id %d, disconnected after %ld seconds. "
                       "%d SQL commands, %d events sent (%lu bytes), binlog '%s', "
                       "last position %lu",
                       router->service->name(),
                       slave->dcb->remote,
                       dcb_get_port(slave->dcb),
                       slave->serverid,
                       time(0) - slave->connect_time,
                       slave->stats.n_queries,
                       slave->stats.n_events,
                       slave->stats.n_bytes,
                       slave->binlog_name,
                       (unsigned long)slave->binlog_pos);
        }
        else
        {
            MXS_NOTICE("%s: Slave %s, server id %d, disconnected after %ld seconds. "
                       "%d SQL commands",
                       router->service->name(),
                       slave->dcb->remote,
                       slave->serverid,
                       time(0) - slave->connect_time,
                       slave->stats.n_queries);
        }

        /*
         * Mark the slave as unregistered to prevent the forwarding
         * of any more binlog records to this slave.
         */
        slave->state = BLRS_UNREGISTERED;

#ifdef BLFILE_IN_SLAVE
        // TODO: Is it really certain the file can be closed here? If other
        // TODO: threads are using the slave instance, bag things will happen. [JWi].
        if (slave->file)
        {
            blr_close_binlog(router, slave->file);
        }
#endif

        /* Unlock */
        rses_end_locked_router_action(slave);
    }
}

/**
 * We have data from the client, this is likely to be packets related to
 * the registration of the slave to receive binlog records. Unlike most
 * MaxScale routers there is no forwarding to the backend database, merely
 * the return of either predefined server responses that have been cached
 * or binlog records.
 *
 * @param instance       The router instance
 * @param router_session The router session returned from the newSession call
 * @param queue          The queue of data buffers to route
 * @return The number of bytes sent
 */
static int routeQuery(MXS_ROUTER* instance, MXS_ROUTER_SESSION* router_session, GWBUF* queue)
{
    ROUTER_INSTANCE* router = (ROUTER_INSTANCE*)instance;
    ROUTER_SLAVE* slave = (ROUTER_SLAVE*)router_session;

    int rc = blr_slave_request(router, slave, queue);
    gwbuf_free(queue);
    return rc;
}

static const char* event_names[] =
{
    "Invalid",                               "Start Event V3",
    "Query Event",
    "Stop Event",                            "Rotate Event",
    "Integer Session Variable",              "Load Event",
    "Slave Event",
    "Create File Event",
    "Append Block Event",                    "Exec Load Event",
    "Delete File Event",
    "New Load Event",                        "Rand Event",
    "User Variable Event",                   "Format Description Event",
    "Transaction ID Event (2 Phase Commit)", "Begin Load Query Event",
    "Execute Load Query Event",              "Table Map Event",
    "Write Rows Event (v0)",
    "Update Rows Event (v0)",                "Delete Rows Event (v0)",
    "Write Rows Event (v1)",
    "Update Rows Event (v1)",                "Delete Rows Event (v1)",
    "Incident Event",
    "Heartbeat Event",                       "Ignorable Event",
    "Rows Query Event",                      "Write Rows Event (v2)",
    "Update Rows Event (v2)",                "Delete Rows Event (v2)",
    "GTID Event",
    "Anonymous GTID Event",                  "Previous GTIDS Event"
};

/* New MariaDB event numbers starts from 0xa0 */
static const char* event_names_mariadb10[] =
{
    "Annotate Rows Event",
    /* New MariaDB 10.x event numbers */
    "Binlog Checkpoint Event",
    "GTID Event",
    "GTID List Event",
    "Start Encryption Event"
};

/**
 * Display an entry from the spinlock statistics data
 *
 * @param   dcb The DCB to print to
 * @param   desc    Description of the statistic
 * @param   value   The statistic value
 */
#if SPINLOCK_PROFILE
static void spin_reporter(void* dcb, char* desc, int value)
{
    dcb_printf((DCB*)dcb, "\t\t%-35s	%d\n", desc, value);
}
#endif

/**
 * Display router diagnostics
 *
 * @param instance  Instance of the router
 * @param dcb       DCB to send diagnostics to
 */
static void diagnostics(MXS_ROUTER* router, DCB* dcb)
{
    ROUTER_INSTANCE* router_inst = (ROUTER_INSTANCE*)router;
    ROUTER_SLAVE* session;
    int i = 0, j;
    int minno = 0;
    double min5, min10, min15, min30;
    char buf[40];
    struct tm tm;

    pthread_mutex_lock(&router_inst->lock);
    session = router_inst->slaves;
    while (session)
    {
        i++;
        session = session->next;
    }
    pthread_mutex_unlock(&router_inst->lock);

    minno = router_inst->stats.minno;
    min30 = 0.0;
    min15 = 0.0;
    min10 = 0.0;
    min5 = 0.0;
    for (j = 0; j < BLR_NSTATS_MINUTES; j++)
    {
        minno--;
        if (minno < 0)
        {
            minno += BLR_NSTATS_MINUTES;
        }
        min30 += router_inst->stats.minavgs[minno];
        if (j < 15)
        {
            min15 += router_inst->stats.minavgs[minno];
        }
        if (j < 10)
        {
            min10 += router_inst->stats.minavgs[minno];
        }
        if (j < 5)
        {
            min5 += router_inst->stats.minavgs[minno];
        }
    }
    min30 /= 30.0;
    min15 /= 15.0;
    min10 /= 10.0;
    min5 /= 5.0;

    if (router_inst->master)
    {
        dcb_printf(dcb,
                   "\tMaster connection DCB:               %p\n",
                   router_inst->master);
    }
    else
    {
        dcb_printf(dcb, "\tMaster connection DCB:               0x0\n");
    }

    /* SSL options */
    if (router_inst->ssl_enabled)
    {
        dcb_printf(dcb, "\tMaster SSL is ON:\n");
        if (router_inst->service->dbref->server && router_inst->service->dbref->server->server_ssl)
        {
            dcb_printf(dcb,
                       "\t\tMaster SSL CA cert: %s\n",
                       router_inst->service->dbref->server->server_ssl->ssl_ca_cert);
            dcb_printf(dcb,
                       "\t\tMaster SSL Cert:    %s\n",
                       router_inst->service->dbref->server->server_ssl->ssl_cert);
            dcb_printf(dcb,
                       "\t\tMaster SSL Key:     %s\n",
                       router_inst->service->dbref->server->server_ssl->ssl_key);
            dcb_printf(dcb,
                       "\t\tMaster SSL tls_ver: %s\n",
                       router_inst->ssl_version ? router_inst->ssl_version : "MAX");
        }
    }

    /* Binlog Encryption options */
    if (router_inst->encryption.enabled)
    {
        dcb_printf(dcb, "\tBinlog Encryption is ON:\n");
        dcb_printf(dcb,
                   "\t\tEncryption Key File:      %s\n",
                   router_inst->encryption.key_management_filename);
        dcb_printf(dcb,
                   "\t\tEncryption Key Algorithm: %s\n",
                   blr_get_encryption_algorithm(router_inst->encryption.encryption_algorithm));
        dcb_printf(dcb,
                   "\t\tEncryption Key length:    %lu bits\n",
                   8 * router_inst->encryption.key_len);
    }

    dcb_printf(dcb,
               "\tMaster connection state:                     %s\n",
               blrm_states[router_inst->master_state]);

    localtime_r(&router_inst->stats.lastReply, &tm);
    asctime_r(&tm, buf);

    dcb_printf(dcb,
               "\tBinlog directory:                            %s\n",
               router_inst->binlogdir);
    dcb_printf(dcb,
               "\tHeartbeat period (seconds):                  %lu%s\n",
               router_inst->heartbeat,
               router_inst->heartbeat ? "" : " (disabled)");
    dcb_printf(dcb,
               "\tNumber of master connects:                   %d\n",
               router_inst->stats.n_masterstarts);
    dcb_printf(dcb,
               "\tNumber of delayed reconnects:                %d\n",
               router_inst->stats.n_delayedreconnects);
    dcb_printf(dcb,
               "\tNumber of connect retries:                   %d\n",
               router_inst->retry_count);
    dcb_printf(dcb,
               "\tConnect retry interval:                      %d\n",
               router_inst->retry_interval);
    dcb_printf(dcb,
               "\tConnect retry count limit:                   %d\n",
               router_inst->retry_limit);
    dcb_printf(dcb,
               "\tCurrent binlog file:                         %s\n",
               router_inst->binlog_name);
    dcb_printf(dcb,
               "\tCurrent binlog position:                     %lu\n",
               router_inst->current_pos);
    if (router_inst->trx_safe)
    {
        if (router_inst->pending_transaction.state != BLRM_NO_TRANSACTION)
        {
            dcb_printf(dcb,
                       "\tCurrent open transaction pos:                %lu\n",
                       router_inst->binlog_position);
        }
    }
    dcb_printf(dcb,
               "\tNumber of slave servers:                     %u\n",
               router_inst->stats.n_slaves);
    dcb_printf(dcb,
               "\tNo. of binlog events received this session:  %lu\n",
               router_inst->stats.n_binlogs_ses);
    dcb_printf(dcb,
               "\tTotal no. of binlog events received:         %lu\n",
               router_inst->stats.n_binlogs);
    dcb_printf(dcb,
               "\tNo. of bad CRC received from master:         %u\n",
               router_inst->stats.n_badcrc);
    minno = router_inst->stats.minno - 1;
    if (minno == -1)
    {
        minno += BLR_NSTATS_MINUTES;
    }
    dcb_printf(dcb, "\tNumber of binlog events per minute\n");
    dcb_printf(dcb, "\tCurrent        5        10       15       30 Min Avg\n");
    dcb_printf(dcb,
               "\t %6d  %8.1f %8.1f %8.1f %8.1f\n",
               router_inst->stats.minavgs[minno],
               min5,
               min10,
               min15,
               min30);
    dcb_printf(dcb,
               "\tNumber of fake binlog events:                %lu\n",
               router_inst->stats.n_fakeevents);
    dcb_printf(dcb,
               "\tNumber of artificial binlog events:          %lu\n",
               router_inst->stats.n_artificial);
    dcb_printf(dcb,
               "\tNumber of binlog events in error:            %lu\n",
               router_inst->stats.n_binlog_errors);
    dcb_printf(dcb,
               "\tNumber of binlog rotate events:              %lu\n",
               router_inst->stats.n_rotates);
    dcb_printf(dcb,
               "\tNumber of heartbeat events:                  %u\n",
               router_inst->stats.n_heartbeats);
    dcb_printf(dcb,
               "\tNumber of packets received:                  %u\n",
               router_inst->stats.n_reads);
    dcb_printf(dcb,
               "\tNumber of residual data packets:             %u\n",
               router_inst->stats.n_residuals);
    dcb_printf(dcb,
               "\tAverage events per packet:                   %.1f\n",
               router_inst->stats.n_reads != 0 ?
               ((double)router_inst->stats.n_binlogs / router_inst->stats.n_reads) : 0);

    pthread_mutex_lock(&router_inst->lock);
    if (router_inst->stats.lastReply)
    {
        if (buf[strlen(buf) - 1] == '\n')
        {
            buf[strlen(buf) - 1] = '\0';
        }
        dcb_printf(dcb,
                   "\tLast event from master at:                   %s (%ld seconds ago)\n",
                   buf,
                   time(0) - router_inst->stats.lastReply);

        if (!router_inst->mariadb10_compat)
        {
            dcb_printf(dcb,
                       "\tLast event from master:                      0x%x, %s\n",
                       router_inst->lastEventReceived,
                       (router_inst->lastEventReceived <= MAX_EVENT_TYPE) ?
                       event_names[router_inst->lastEventReceived] : "unknown");
        }
        else
        {
            const char* ptr = NULL;
            if (router_inst->lastEventReceived <= MAX_EVENT_TYPE)
            {
                ptr = event_names[router_inst->lastEventReceived];
            }
            else
            {
                /* Check MariaDB 10 new events */
                if (router_inst->lastEventReceived >= MARIADB_NEW_EVENTS_BEGIN
                    && router_inst->lastEventReceived <= MAX_EVENT_TYPE_MARIADB10)
                {
                    ptr = event_names_mariadb10[(router_inst->lastEventReceived - MARIADB_NEW_EVENTS_BEGIN)];
                }
            }

            dcb_printf(dcb,
                       "\tLast event from master:                      0x%x, %s\n",
                       router_inst->lastEventReceived,
                       (ptr != NULL) ? ptr : "unknown");

            if (router_inst->mariadb10_gtid
                && router_inst->last_mariadb_gtid[0])
            {
                dcb_printf(dcb,
                           "\tLast seen MariaDB GTID:                      %s\n",
                           router_inst->last_mariadb_gtid);
            }
        }

        if (router_inst->lastEventTimestamp)
        {
            time_t last_event = (time_t)router_inst->lastEventTimestamp;
            localtime_r(&last_event, &tm);
            asctime_r(&tm, buf);
            if (buf[strlen(buf) - 1] == '\n')
            {
                buf[strlen(buf) - 1] = '\0';
            }
            dcb_printf(dcb,
                       "\tLast binlog event timestamp:                 %u (%s)\n",
                       router_inst->lastEventTimestamp,
                       buf);
        }
    }
    else
    {
        dcb_printf(dcb, "\tNo events received from master yet\n");
    }
    pthread_mutex_unlock(&router_inst->lock);

    if (router_inst->active_logs)
    {
        dcb_printf(dcb, "\tRouter processing binlog records\n");
    }
    if (router_inst->reconnect_pending)
    {
        dcb_printf(dcb, "\tRouter pending reconnect to master\n");
    }
    dcb_printf(dcb, "\tEvents received:\n");
    for (i = 0; i <= MAX_EVENT_TYPE; i++)
    {
        dcb_printf(dcb, "\t\t%-38s   %lu\n", event_names[i], router_inst->stats.events[i]);
    }

    if (router_inst->mariadb10_compat)
    {
        /* Display MariaDB 10 new events */
        for (i = MARIADB_NEW_EVENTS_BEGIN; i <= MAX_EVENT_TYPE_MARIADB10; i++)
        {
            dcb_printf(dcb,
                       "\t\tMariaDB 10 %-38s   %lu\n",
                       event_names_mariadb10[(i - MARIADB_NEW_EVENTS_BEGIN)],
                       router_inst->stats.events[i]);
        }
    }

#if SPINLOCK_PROFILE
    dcb_printf(dcb, "\tSpinlock statistics (instlock):\n");
    spinlock_stats(&instlock, spin_reporter, dcb);
    dcb_printf(dcb, "\tSpinlock statistics (instance lock):\n");
    spinlock_stats(&router_inst->lock, spin_reporter, dcb);
    dcb_printf(dcb, "\tSpinlock statistics (binlog position lock):\n");
    spinlock_stats(&router_inst->binlog_lock, spin_reporter, dcb);
#endif

    if (router_inst->slaves)
    {
        dcb_printf(dcb, "\tSlaves:\n");
        pthread_mutex_lock(&router_inst->lock);
        session = router_inst->slaves;
        while (session)
        {

            minno = session->stats.minno;
            min30 = 0.0;
            min15 = 0.0;
            min10 = 0.0;
            min5 = 0.0;
            for (j = 0; j < BLR_NSTATS_MINUTES; j++)
            {
                minno--;
                if (minno < 0)
                {
                    minno += BLR_NSTATS_MINUTES;
                }
                min30 += session->stats.minavgs[minno];
                if (j < 15)
                {
                    min15 += session->stats.minavgs[minno];
                }
                if (j < 10)
                {
                    min10 += session->stats.minavgs[minno];
                }
                if (j < 5)
                {
                    min5 += session->stats.minavgs[minno];
                }
            }
            min30 /= 30.0;
            min15 /= 15.0;
            min10 /= 10.0;
            min5 /= 5.0;
            dcb_printf(dcb,
                       "\t\tServer-id:                               %d\n",
                       session->serverid);
            if (session->hostname)
            {
                dcb_printf(dcb,
                           "\t\tHostname:                                %s\n",
                           session->hostname);
            }
            if (session->uuid)
            {
                dcb_printf(dcb, "\t\tSlave UUID:                              %s\n", session->uuid);
            }
            dcb_printf(dcb,
                       "\t\tSlave_host_port:                         [%s]:%d\n",
                       session->dcb->remote,
                       dcb_get_port(session->dcb));
            dcb_printf(dcb,
                       "\t\tUsername:                                %s\n",
                       session->dcb->user);
            dcb_printf(dcb,
                       "\t\tSlave DCB:                               %p\n",
                       session->dcb);
            if (session->dcb->ssl)
            {
                dcb_printf(dcb,
                           "\t\tSlave connected with SSL:                %s\n",
                           session->dcb->ssl_state == SSL_ESTABLISHED ?
                           "Established" : "Not connected yet");
            }
            dcb_printf(dcb,
                       "\t\tNext Sequence No:                        %d\n",
                       session->seqno);
            dcb_printf(dcb,
                       "\t\tState:                                   %s\n",
                       blrs_states[session->state]);
            dcb_printf(dcb,
                       "\t\tBinlog file:                             %s\n",
                       session->binlog_name);
            dcb_printf(dcb,
                       "\t\tBinlog position:                         %u\n",
                       session->binlog_pos);
            if (session->nocrc)
            {
                dcb_printf(dcb,
                           "\t\tMaster Binlog CRC:                       None\n");
            }
            dcb_printf(dcb,
                       "\t\tNo. requests:                            %u\n",
                       session->stats.n_requests);
            dcb_printf(dcb,
                       "\t\tNo. events sent:                         %u\n",
                       session->stats.n_events);
            dcb_printf(dcb,
                       "\t\tNo. bytes sent:                          %lu\n",
                       session->stats.n_bytes);
            dcb_printf(dcb,
                       "\t\tNo. bursts sent:                         %u\n",
                       session->stats.n_bursts);
            dcb_printf(dcb,
                       "\t\tNo. transitions to follow mode:          %u\n",
                       session->stats.n_bursts);
            if (router_inst->send_slave_heartbeat)
            {
                dcb_printf(dcb,
                           "\t\tHeartbeat period (seconds):              %d\n",
                           session->heartbeat);
            }

            minno = session->stats.minno - 1;
            if (minno == -1)
            {
                minno += BLR_NSTATS_MINUTES;
            }
            dcb_printf(dcb, "\t\tNumber of binlog events per minute\n");
            dcb_printf(dcb, "\t\tCurrent        5        10       15       30 Min Avg\n");
            dcb_printf(dcb,
                       "\t\t %6d  %8.1f %8.1f %8.1f %8.1f\n",
                       session->stats.minavgs[minno],
                       min5,
                       min10,
                       min15,
                       min30);
            dcb_printf(dcb,
                       "\t\tNo. flow control:                        %u\n",
                       session->stats.n_flows);
            dcb_printf(dcb,
                       "\t\tNo. up to date:                          %u\n",
                       session->stats.n_upd);
            dcb_printf(dcb,
                       "\t\tNo. of drained cbs                       %u\n",
                       session->stats.n_dcb);
            dcb_printf(dcb,
                       "\t\tNo. of failed reads                      %u\n",
                       session->stats.n_failed_read);

#ifdef DETAILED_DIAG
            dcb_printf(dcb,
                       "\t\tNo. of nested distribute events          %u\n",
                       session->stats.n_overrun);
            dcb_printf(dcb,
                       "\t\tNo. of distribute action 1               %u\n",
                       session->stats.n_actions[0]);
            dcb_printf(dcb,
                       "\t\tNo. of distribute action 2               %u\n",
                       session->stats.n_actions[1]);
            dcb_printf(dcb,
                       "\t\tNo. of distribute action 3               %u\n",
                       session->stats.n_actions[2]);
#endif
            if (session->lastEventTimestamp
                && router_inst->lastEventTimestamp && session->lastEventReceived != HEARTBEAT_EVENT)
            {
                unsigned long seconds_behind;
                time_t session_last_event = (time_t)session->lastEventTimestamp;

                if (router_inst->lastEventTimestamp > session->lastEventTimestamp)
                {
                    seconds_behind = router_inst->lastEventTimestamp - session->lastEventTimestamp;
                }
                else
                {
                    seconds_behind = 0;
                }

                localtime_r(&session_last_event, &tm);
                asctime_r(&tm, buf);
                dcb_printf(dcb,
                           "\t\tLast binlog event timestamp              %u, %s",
                           session->lastEventTimestamp,
                           buf);
                dcb_printf(dcb,
                           "\t\tSeconds behind master                    %lu\n",
                           seconds_behind);
            }

            if (session->state == 0)
            {
                dcb_printf(dcb, "\t\tSlave_mode:                              connected\n");
            }
            else
            {
                if ((session->cstate & CS_WAIT_DATA) == CS_WAIT_DATA)
                {
                    dcb_printf(dcb, "\t\tSlave_mode:                              wait-for-data\n");
                }
                else
                {
                    dcb_printf(dcb,
                               "\t\tSlave_mode:                              catchup. %s%s\n",
                               ((session->cstate & CS_EXPECTCB) == 0 ? "" :
                                "Waiting for DCB queue to drain."),
                               ((session->cstate & CS_BUSY) == 0 ? "" :
                                " Busy in slave catchup."));
                }
            }
#if SPINLOCK_PROFILE
            dcb_printf(dcb, "\tSpinlock statistics (catch_lock):\n");
            spinlock_stats(&session->catch_lock, spin_reporter, dcb);
            dcb_printf(dcb, "\tSpinlock statistics (rses_lock):\n");
            spinlock_stats(&session->rses_lock, spin_reporter, dcb);
#endif
            dcb_printf(dcb, "\t\t--------------------\n\n");
            session = session->next;
        }
        pthread_mutex_unlock(&router_inst->lock);
    }
}

/**
 * Display router diagnostics
 *
 * @param instance  Instance of the router
 */
static json_t* diagnostics_json(const MXS_ROUTER* router)
{
    ROUTER_INSTANCE* router_inst = (ROUTER_INSTANCE*)router;
    int minno = 0;
    double min5, min10, min15, min30;
    char buf[40];
    struct tm tm;

    json_t* rval = json_object();

    minno = router_inst->stats.minno;
    min30 = 0.0;
    min15 = 0.0;
    min10 = 0.0;
    min5 = 0.0;
    for (int j = 0; j < BLR_NSTATS_MINUTES; j++)
    {
        minno--;
        if (minno < 0)
        {
            minno += BLR_NSTATS_MINUTES;
        }
        min30 += router_inst->stats.minavgs[minno];
        if (j < 15)
        {
            min15 += router_inst->stats.minavgs[minno];
        }
        if (j < 10)
        {
            min10 += router_inst->stats.minavgs[minno];
        }
        if (j < 5)
        {
            min5 += router_inst->stats.minavgs[minno];
        }
    }
    min30 /= 30.0;
    min15 /= 15.0;
    min10 /= 10.0;
    min5 /= 5.0;

    /* SSL options */
    if (router_inst->ssl_enabled)
    {
        json_t* obj = json_object();

        json_object_set_new(obj,
                            "ssl_ca_cert",
                            json_string(router_inst->service->dbref->server->server_ssl->ssl_ca_cert));
        json_object_set_new(obj,
                            "ssl_cert",
                            json_string(router_inst->service->dbref->server->server_ssl->ssl_cert));
        json_object_set_new(obj,
                            "ssl_key",
                            json_string(router_inst->service->dbref->server->server_ssl->ssl_key));
        json_object_set_new(obj,
                            "ssl_version",
                            json_string(router_inst->ssl_version ? router_inst->ssl_version : "MAX"));

        json_object_set_new(rval, "master_ssl", obj);
    }

    /* Binlog Encryption options */
    if (router_inst->encryption.enabled)
    {
        json_t* obj = json_object();

        json_object_set_new(obj,
                            "key",
                            json_string(router_inst->encryption.key_management_filename));
        json_object_set_new(obj,
                            "algorithm",
                            json_string(blr_get_encryption_algorithm(router_inst->encryption.
                                                                     encryption_algorithm)));
        json_object_set_new(obj,
                            "key_length",
                            json_integer(8 * router_inst->encryption.key_len));

        json_object_set_new(rval, "master_encryption", obj);
    }

    json_object_set_new(rval, "master_state", json_string(blrm_states[router_inst->master_state]));

    localtime_r(&router_inst->stats.lastReply, &tm);
    asctime_r(&tm, buf);


    json_object_set_new(rval, "binlogdir", json_string(router_inst->binlogdir));
    json_object_set_new(rval, "heartbeat", json_integer(router_inst->heartbeat));
    json_object_set_new(rval, "master_retry_interval", json_integer(router_inst->retry_interval));
    json_object_set_new(rval, "master_retry_limit", json_integer(router_inst->retry_limit));
    json_object_set_new(rval, "master_retries", json_integer(router_inst->retry_count));
    json_object_set_new(rval, "master_starts", json_integer(router_inst->stats.n_masterstarts));
    json_object_set_new(rval, "master_reconnects", json_integer(router_inst->stats.n_delayedreconnects));
    json_object_set_new(rval, "binlog_name", json_string(router_inst->binlog_name));
    json_object_set_new(rval, "binlog_position", json_integer(router_inst->current_pos));

    if (router_inst->trx_safe)
    {
        if (router_inst->pending_transaction.state != BLRM_NO_TRANSACTION)
        {
            json_object_set_new(rval, "current_trx_position", json_integer(router_inst->binlog_position));
        }
    }

    json_object_set_new(rval, "slaves", json_integer(router_inst->stats.n_slaves));
    json_object_set_new(rval, "session_events", json_integer(router_inst->stats.n_binlogs_ses));
    json_object_set_new(rval, "total_events", json_integer(router_inst->stats.n_binlogs));
    json_object_set_new(rval, "bad_crc_count", json_integer(router_inst->stats.n_badcrc));

    minno = router_inst->stats.minno - 1;
    if (minno == -1)
    {
        minno += BLR_NSTATS_MINUTES;
    }

    json_object_set_new(rval, "events_0", json_real(router_inst->stats.minavgs[minno]));
    json_object_set_new(rval, "events_5", json_real(min5));
    json_object_set_new(rval, "events_10", json_real(min10));
    json_object_set_new(rval, "events_15", json_real(min15));
    json_object_set_new(rval, "events_30", json_real(min30));

    json_object_set_new(rval, "fake_events", json_integer(router_inst->stats.n_fakeevents));

    json_object_set_new(rval, "artificial_events", json_integer(router_inst->stats.n_artificial));

    json_object_set_new(rval, "binlog_errors", json_integer(router_inst->stats.n_binlog_errors));
    json_object_set_new(rval, "binlog_rotates", json_integer(router_inst->stats.n_rotates));
    json_object_set_new(rval, "heartbeat_events", json_integer(router_inst->stats.n_heartbeats));
    json_object_set_new(rval, "events_read", json_integer(router_inst->stats.n_reads));
    json_object_set_new(rval, "residual_packets", json_integer(router_inst->stats.n_residuals));

    double average_packets = router_inst->stats.n_reads != 0 ?
        ((double)router_inst->stats.n_binlogs / router_inst->stats.n_reads) : 0;

    json_object_set_new(rval, "average_events_per_packets", json_real(average_packets));

    pthread_mutex_lock(&router_inst->lock);
    if (router_inst->stats.lastReply)
    {
        if (buf[strlen(buf) - 1] == '\n')
        {
            buf[strlen(buf) - 1] = '\0';
        }

        json_object_set_new(rval, "latest_event", json_string(buf));

        if (!router_inst->mariadb10_compat)
        {
            json_object_set_new(rval,
                                "latest_event_type",
                                json_string((router_inst->lastEventReceived <= MAX_EVENT_TYPE) ?
                                            event_names[router_inst->lastEventReceived] :
                                            "unknown"));
        }
        else
        {
            const char* ptr = NULL;
            if (router_inst->lastEventReceived <= MAX_EVENT_TYPE)
            {
                ptr = event_names[router_inst->lastEventReceived];
            }
            else
            {
                /* Check MariaDB 10 new events */
                if (router_inst->lastEventReceived >= MARIADB_NEW_EVENTS_BEGIN
                    && router_inst->lastEventReceived <= MAX_EVENT_TYPE_MARIADB10)
                {
                    ptr = event_names_mariadb10[(router_inst->lastEventReceived - MARIADB_NEW_EVENTS_BEGIN)];
                }
            }


            json_object_set_new(rval, "latest_event_type", json_string((ptr != NULL) ? ptr : "unknown"));

            if (router_inst->mariadb10_gtid
                && router_inst->last_mariadb_gtid[0])
            {
                json_object_set_new(rval, "latest_gtid", json_string(router_inst->last_mariadb_gtid));
            }
        }

        if (router_inst->lastEventTimestamp)
        {
            time_t last_event = (time_t)router_inst->lastEventTimestamp;
            localtime_r(&last_event, &tm);
            asctime_r(&tm, buf);
            if (buf[strlen(buf) - 1] == '\n')
            {
                buf[strlen(buf) - 1] = '\0';
            }

            json_object_set_new(rval, "latest_event_timestamp", json_string(buf));
        }
    }
    pthread_mutex_unlock(&router_inst->lock);

    json_object_set_new(rval, "active_logs", json_boolean(router_inst->active_logs));
    json_object_set_new(rval, "reconnect_pending", json_boolean(router_inst->reconnect_pending));

    json_t* ev = json_object();

    for (int i = 0; i <= MAX_EVENT_TYPE; i++)
    {
        json_object_set_new(ev, event_names[i], json_integer(router_inst->stats.events[i]));
    }

    if (router_inst->mariadb10_compat)
    {
        /* Display MariaDB 10 new events */
        for (int i = MARIADB_NEW_EVENTS_BEGIN; i <= MAX_EVENT_TYPE_MARIADB10; i++)
        {
            json_object_set_new(ev,
                                event_names_mariadb10[(i - MARIADB_NEW_EVENTS_BEGIN)],
                                json_integer(router_inst->stats.events[i]));
        }
    }

    json_object_set_new(rval, "event_types", ev);

    if (router_inst->slaves)
    {
        json_t* arr = json_array();
        pthread_mutex_lock(&router_inst->lock);

        for (ROUTER_SLAVE* session = router_inst->slaves; session; session = session->next)
        {
            json_t* slave = json_object();
            minno = session->stats.minno;
            min30 = 0.0;
            min15 = 0.0;
            min10 = 0.0;
            min5 = 0.0;
            for (int j = 0; j < BLR_NSTATS_MINUTES; j++)
            {
                minno--;
                if (minno < 0)
                {
                    minno += BLR_NSTATS_MINUTES;
                }
                min30 += session->stats.minavgs[minno];
                if (j < 15)
                {
                    min15 += session->stats.minavgs[minno];
                }
                if (j < 10)
                {
                    min10 += session->stats.minavgs[minno];
                }
                if (j < 5)
                {
                    min5 += session->stats.minavgs[minno];
                }
            }
            min30 /= 30.0;
            min15 /= 15.0;
            min10 /= 10.0;
            min5 /= 5.0;

            json_object_set_new(rval, "server_id", json_integer(session->serverid));

            if (session->hostname)
            {
                json_object_set_new(rval, "hostname", json_string(session->hostname));
            }
            if (session->uuid)
            {
                json_object_set_new(rval, "uuid", json_string(session->uuid));
            }

            json_object_set_new(rval, "address", json_string(session->dcb->remote));
            json_object_set_new(rval, "port", json_integer(dcb_get_port(session->dcb)));
            json_object_set_new(rval, "user", json_string(session->dcb->user));
            json_object_set_new(rval, "ssl_enabled", json_boolean(session->dcb->ssl));
            json_object_set_new(rval, "state", json_string(blrs_states[session->state]));
            json_object_set_new(rval, "next_sequence", json_integer(session->seqno));
            json_object_set_new(rval, "binlog_file", json_string(session->binlog_name));
            json_object_set_new(rval, "binlog_pos", json_integer(session->binlog_pos));
            json_object_set_new(rval, "crc", json_boolean(!session->nocrc));

            json_object_set_new(rval, "requests", json_integer(session->stats.n_requests));
            json_object_set_new(rval, "events_sent", json_integer(session->stats.n_events));
            json_object_set_new(rval, "bytes_sent", json_integer(session->stats.n_bytes));
            json_object_set_new(rval, "data_bursts", json_integer(session->stats.n_bursts));

            if (router_inst->send_slave_heartbeat)
            {
                json_object_set_new(rval, "heartbeat_period", json_integer(session->heartbeat));
            }

            minno = session->stats.minno - 1;
            if (minno == -1)
            {
                minno += BLR_NSTATS_MINUTES;
            }

            if (session->lastEventTimestamp
                && router_inst->lastEventTimestamp && session->lastEventReceived != HEARTBEAT_EVENT)
            {
                unsigned long seconds_behind;
                time_t session_last_event = (time_t)session->lastEventTimestamp;

                if (router_inst->lastEventTimestamp > session->lastEventTimestamp)
                {
                    seconds_behind = router_inst->lastEventTimestamp - session->lastEventTimestamp;
                }
                else
                {
                    seconds_behind = 0;
                }

                localtime_r(&session_last_event, &tm);
                asctime_r(&tm, buf);
                mxb::trim(buf);
                json_object_set_new(rval, "last_binlog_event_timestamp", json_string(buf));
                json_object_set_new(rval, "seconds_behind_master", json_integer(seconds_behind));
            }

            const char* mode = "connected";

            if (session->state)
            {
                if ((session->cstate & CS_WAIT_DATA) == CS_WAIT_DATA)
                {
                    mode = "wait-for-data";
                }
                else
                {
                    mode = "catchup";
                }
            }

            json_object_set_new(slave, "mode", json_string(mode));

            json_array_append_new(arr, slave);
        }
        pthread_mutex_unlock(&router_inst->lock);

        json_object_set_new(rval, "slaves", arr);
    }

    return rval;
}

/**
 * Client Reply routine - in this case this is a message from the
 * master server, It should be sent to the state machine that manages
 * master packets as it may be binlog records or part of the registration
 * handshake that takes part during connection establishment.
 *
 *
 * @param       instance        The router instance
 * @param       router_session  The router session
 * @param       master_dcb      The DCB for the connection to the master
 * @param       queue           The GWBUF with reply data
 */
static void clientReply(MXS_ROUTER* instance,
                        MXS_ROUTER_SESSION* router_session,
                        GWBUF* queue,
                        DCB*   backend_dcb)
{
    ROUTER_INSTANCE* router = (ROUTER_INSTANCE*)instance;

    atomic_add(&router->stats.n_reads, 1);
    blr_master_response(router, queue);
    router->stats.lastReply = time(0);
}

static char* extract_message(GWBUF* errpkt)
{
    char* rval;
    int len;

    len = EXTRACT24(errpkt->start);
    if ((rval = (char*)MXS_MALLOC(len)) == NULL)
    {
        return NULL;
    }
    memcpy(rval, (char*)(errpkt->start) + 7, 6);
    rval[6] = ' ';
    /* message size is len - (1 byte field count + 2 bytes errno + 6 bytes status) */
    memcpy(&rval[7], (char*)(errpkt->start) + 13, len - 9);
    rval[len - 2] = 0;
    return rval;
}



/**
 * Error Reply routine
 *
 * The routine will reply to client errors and/or closing the session
 * or try to open a new backend connection.
 *
 * @param       instance        The router instance
 * @param       router_session  The router session
 * @param       message         The error message to reply
 * @param       backend_dcb     The backend DCB
 * @param       action      The action: ERRACT_NEW_CONNECTION or ERRACT_REPLY_CLIENT
 * @param   succp       Result of action: true iff router can continue
 *
 */
static void errorReply(MXS_ROUTER* instance,
                       MXS_ROUTER_SESSION* router_session,
                       GWBUF* message,
                       DCB*   backend_dcb,
                       mxs_error_action_t action,
                       bool* succp)
{
    mxb_assert(backend_dcb->role == DCB::Role::BACKEND);
    ROUTER_INSTANCE* router = (ROUTER_INSTANCE*)instance;
    int error;
    socklen_t len;
    char msg[MXS_STRERROR_BUFLEN + 1 + 5] = "";
    char* errmsg;
    unsigned long mysql_errno;

    mysql_errno = (unsigned long) extract_field(((uint8_t*)GWBUF_DATA(message) + 5), 16);
    errmsg = extract_message(message);

    if (action == ERRACT_REPLY_CLIENT)
    {
        /** Check router state and set errno and message */
        if (router->master_state != BLRM_SLAVE_STOPPED)
        {
            /* Authentication failed: stop replication */
            if (router->master_state == BLRM_TIMESTAMP)
            {
                pthread_mutex_lock(&router->lock);
                /* set io error message */
                if (router->m_errmsg)
                {
                    free(router->m_errmsg);
                }
                router->m_errmsg = mxb_strdup("#28000 Authentication with master server failed");
                /* set mysql_errno */
                router->m_errno = 1045;

                /* Stop replication */
                router->master_state = BLRM_SLAVE_STOPPED;
                pthread_mutex_unlock(&router->lock);

                /* Force backend DCB close */
                dcb_close(backend_dcb);

                /* Force Fake Client DCB close */
                if (router->client)
                {
                    dcb_close(router->client);
                    router->client = NULL;
                }

                MXS_ERROR("%s: Master connection error %lu '%s' in state '%s', "
                          "%s while connecting to master [%s]:%d. Replication is stopped.",
                          router->service->name(),
                          router->m_errno,
                          router->m_errmsg,
                          blrm_states[BLRM_TIMESTAMP],
                          msg,
                          router->service->dbref->server->address,
                          router->service->dbref->server->port);
            }
        }
        if (errmsg)
        {
            MXS_FREE(errmsg);
        }

        *succp = true;
        return;
    }

    len = sizeof(error);
    if (router->master
        && getsockopt(router->master->fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0
        && error != 0)
    {
        sprintf(msg, "%s, ", mxs_strerror(error));
    }
    else
    {
        strcpy(msg, "");
    }

    /** Check router state and set errno and message */
    if (router->master_state != BLRM_SLAVE_STOPPED)
    {
        pthread_mutex_lock(&router->lock);
        /* set mysql_errno */
        router->m_errno = mysql_errno;

        /* set io error message */
        if (router->m_errmsg)
        {
            MXS_FREE(router->m_errmsg);
        }
        router->m_errmsg = MXS_STRDUP_A(errmsg);
        pthread_mutex_unlock(&router->lock);

        MXS_ERROR("%s: Master connection error %lu '%s' in state '%s', "
                  "%sattempting reconnect to master [%s]:%d",
                  router->service->name(),
                  mysql_errno,
                  errmsg,
                  blrm_states[router->master_state],
                  msg,
                  router->service->dbref->server->address,
                  router->service->dbref->server->port);
    }
    else
    {
        /* Stopped state, no reconnection */
        MXS_INFO("%s: Master connection has been closed. State is '%s', "
                 "%snot retrying a new connection to master [%s]:%d",
                 router->service->name(),
                 blrm_states[router->master_state],
                 msg,
                 router->service->dbref->server->address,
                 router->service->dbref->server->port);
    }

    if (errmsg)
    {
        MXS_FREE(errmsg);
    }
    *succp = true;

    /* Force Backend DCB close */
    if (backend_dcb == router->master)
    {
        router->master = NULL;
    }
    dcb_close(backend_dcb);

    /* Force Fake Client DCB close */
    if (router->client)
    {
        dcb_close(router->client);
        router->client = NULL;
    }

    MXS_NOTICE("%s: Master %s disconnected after %ld seconds. "
               "%lu events read.",
               router->service->name(),
               router->service->dbref->server->address,
               time(0) - router->connect_time,
               router->stats.n_binlogs_ses);
    blr_master_reconnect(router);
}

/** to be inline'd */
/**
 * @node Acquires lock to router client session if it is not closed.
 *
 * Parameters:
 * @param rses - in, use
 *
 *
 * @return true if router session was not closed. If return value is true
 * it means that router is locked, and must be unlocked later. False, if
 * router was closed before lock was acquired.
 *
 *
 * @details (write detailed description here)
 *
 */
static bool rses_begin_locked_router_action(ROUTER_SLAVE* rses)
{
    bool succp = false;

    pthread_mutex_lock(&rses->rses_lock);
    succp = true;

    return succp;
}

/** to be inline'd */
/**
 * @node Releases router client session lock.
 *
 * Parameters:
 * @param rses - <usage>
 *          <description>
 *
 * @return void
 *
 *
 * @details (write detailed description here)
 *
 */
static void rses_end_locked_router_action(ROUTER_SLAVE* rses)
{
    pthread_mutex_unlock(&rses->rses_lock);
}


static uint64_t getCapabilities(MXS_ROUTER* instance)
{
    return RCAP_TYPE_NONE;
}

/**
 * The stats gathering function called from the housekeeper so that we
 * can get timed averages of binlog records shippped
 *
 * @param inst  The router instance
 */
static bool stats_func(void* inst)
{
    ROUTER_INSTANCE* router = (ROUTER_INSTANCE*)inst;
    ROUTER_SLAVE* slave;

    router->stats.minavgs[router->stats.minno++] = router->stats.n_binlogs - router->stats.lastsample;
    router->stats.lastsample = router->stats.n_binlogs;
    if (router->stats.minno == BLR_NSTATS_MINUTES)
    {
        router->stats.minno = 0;
    }

    pthread_mutex_lock(&router->lock);
    slave = router->slaves;
    while (slave)
    {
        slave->stats.minavgs[slave->stats.minno++] = slave->stats.n_events - slave->stats.lastsample;
        slave->stats.lastsample = slave->stats.n_events;
        if (slave->stats.minno == BLR_NSTATS_MINUTES)
        {
            slave->stats.minno = 0;
        }
        slave = slave->next;
    }
    pthread_mutex_unlock(&router->lock);

    return true;
}

/**
 * Return some basic statistics from the router in response to a COM_STATISTICS
 * request.
 *
 * @param router    The router instance
 * @param slave     The "slave" connection that requested the statistics
 * @param queue     The statistics request
 *
 * @return non-zero on sucessful send
 */
int blr_statistics(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave, GWBUF* queue)
{
    char result[BLRM_COM_STATISTICS_SIZE + 1] = "";
    uint8_t* ptr;
    GWBUF* ret;
    unsigned long len;

    snprintf(result,
             BLRM_COM_STATISTICS_SIZE,
             "Uptime: %u  Threads: %u  Events: %u  Slaves: %u  Master State: %s",
             (unsigned int)(time(0) - router->connect_time),
             (unsigned int)config_threadcount(),
             (unsigned int)router->stats.n_binlogs_ses,
             (unsigned int)router->stats.n_slaves,
             blrm_states[router->master_state]);
    if ((ret = gwbuf_alloc(4 + strlen(result))) == NULL)
    {
        return 0;
    }
    len = strlen(result);
    ptr = GWBUF_DATA(ret);
    *ptr++ = len & 0xff;
    *ptr++ = (len & 0xff00) >> 8;
    *ptr++ = (len & 0xff0000) >> 16;
    *ptr++ = 1;
    memcpy(ptr, result, len);

    return MXS_SESSION_ROUTE_REPLY(slave->dcb->session, ret);
}

/**
 * Respond to a COM_PING command
 *
 * @param router    The router instance
 * @param slave     The "slave" connection that requested the ping
 * @param queue     The ping request
 */
int blr_ping(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave, GWBUF* queue)
{
    uint8_t* ptr;
    GWBUF* ret;

    if ((ret = gwbuf_alloc(5)) == NULL)
    {
        return 0;
    }
    ptr = GWBUF_DATA(ret);
    *ptr++ = 0x01;
    *ptr++ = 0;
    *ptr++ = 0;
    *ptr++ = 1;
    *ptr = 0;       // OK

    return MXS_SESSION_ROUTE_REPLY(slave->dcb->session, ret);
}



/**
 * mysql_send_custom_error
 *
 * Send a MySQL protocol Generic ERR message, to the dcb
 *
 * @param dcb Owner_Dcb Control Block for the connection to which the error message is sent
 * @param packet_number
 * @param in_affected_rows
 * @param msg       Message to send
 * @param statemsg  MySQL State message
 * @param errcode   MySQL Error code
 * @return 1 Non-zero if data was sent
 *
 */
int blr_send_custom_error(DCB* dcb,
                          int  packet_number,
                          int  affected_rows,
                          const char* msg,
                          const char* statemsg,
                          unsigned int errcode)
{
    uint8_t* outbuf = NULL;
    uint32_t mysql_payload_size = 0;
    uint8_t mysql_packet_header[4];
    uint8_t* mysql_payload = NULL;
    uint8_t field_count = 0;
    uint8_t mysql_err[2];
    uint8_t mysql_statemsg[6];
    unsigned int mysql_errno = 0;
    const char* mysql_error_msg = NULL;
    const char* mysql_state = NULL;
    GWBUF* errbuf = NULL;

    if (errcode == 0)
    {
        mysql_errno = 1064;
    }
    else
    {
        mysql_errno = errcode;
    }

    mysql_error_msg = "An errorr occurred ...";
    if (statemsg == NULL)
    {
        mysql_state = "42000";
    }
    else
    {
        mysql_state = statemsg;
    }

    field_count = 0xff;
    gw_mysql_set_byte2(mysql_err, mysql_errno);
    mysql_statemsg[0] = '#';
    memcpy(mysql_statemsg + 1, mysql_state, 5);

    if (msg != NULL)
    {
        mysql_error_msg = msg;
    }

    mysql_payload_size = sizeof(field_count)
        + sizeof(mysql_err)
        + sizeof(mysql_statemsg)
        + strlen(mysql_error_msg);

    /** allocate memory for packet header + payload */
    errbuf = gwbuf_alloc(sizeof(mysql_packet_header) + mysql_payload_size);
    mxb_assert(errbuf != NULL);

    if (errbuf == NULL)
    {
        return 0;
    }
    outbuf = GWBUF_DATA(errbuf);

    /** write packet header and packet number */
    gw_mysql_set_byte3(mysql_packet_header, mysql_payload_size);
    mysql_packet_header[3] = packet_number;

    /** write header */
    memcpy(outbuf, mysql_packet_header, sizeof(mysql_packet_header));

    mysql_payload = outbuf + sizeof(mysql_packet_header);

    /** write field */
    memcpy(mysql_payload, &field_count, sizeof(field_count));
    mysql_payload = mysql_payload + sizeof(field_count);

    /** write errno */
    memcpy(mysql_payload, mysql_err, sizeof(mysql_err));
    mysql_payload = mysql_payload + sizeof(mysql_err);

    /** write sqlstate */
    memcpy(mysql_payload, mysql_statemsg, sizeof(mysql_statemsg));
    mysql_payload = mysql_payload + sizeof(mysql_statemsg);

    /** write error message */
    memcpy(mysql_payload, mysql_error_msg, strlen(mysql_error_msg));

    return MXS_SESSION_ROUTE_REPLY(dcb->session, errbuf);
}

/**
 * Extract a numeric field from a packet of the specified number of bits
 *
 * @param src   The raw packet source
 * @param birs  The number of bits to extract (multiple of 8)
 */
uint32_t extract_field(uint8_t* src, int bits)
{
    uint32_t rval = 0, shift = 0;

    while (bits > 0)
    {
        rval |= (*src++) << shift;
        shift += 8;
        bits -= 8;
    }
    return rval;
}

/**
 * Check whether current binlog is valid.
 * In case of errors BLR_SLAVE_STOPPED state is set
 * If a partial transaction is found
 * inst->binlog_position is set the pos where it started
 *
 * @param router    The router instance
 * @return      1 on success, 0 on failure
 */
/** 1 is succes, 0 is faulure */
static int blr_check_binlog(ROUTER_INSTANCE* router)
{
    int n;

    /** blr_read_events_all() may set master_state
     * to BLR_SLAVE_STOPPED state in case of found errors.
     * In such conditions binlog file is NOT truncated and
     * router state is set to BLR_SLAVE_STOPPED
     * Last commited pos is set for both router->binlog_position
     * and router->current_pos.
     *
     * If an open transaction is detected at pos XYZ
     * inst->binlog_position will be set to XYZ while
     * router->current_pos is the last event found.
     */

    n = blr_read_events_all_events(router, NULL, 0);

    MXS_DEBUG("blr_read_events_all_events() ret = %i\n", n);

    if (n != 0)
    {
        char msg_err[BINLOG_ERROR_MSG_LEN + 1] = "";
        router->master_state = BLRM_SLAVE_STOPPED;

        snprintf(msg_err,
                 BINLOG_ERROR_MSG_LEN,
                 "Error found in binlog %s. Safe pos is %lu",
                 router->binlog_name,
                 router->binlog_position);
        /* set mysql_errno */
        if (!router->m_errno)
        {
            router->m_errno = 2032;
        }

        /* set io error message */
        router->m_errmsg = MXS_STRDUP_A(msg_err);

        /* set last_safe_pos */
        router->last_safe_pos = router->binlog_position;

        MXS_ERROR("Error found in binlog file %s. Safe starting pos is %lu",
                  router->binlog_name,
                  router->binlog_position);

        return 0;
    }
    else
    {
        return 1;
    }
}


/**
 * Return last event description
 *
 * @param router    The router instance
 * @return      The event description or NULL
 */
const char* blr_last_event_description(ROUTER_INSTANCE* router)
{
    const char* event_desc = NULL;

    if (!router->mariadb10_compat)
    {
        if (router->lastEventReceived <= MAX_EVENT_TYPE)
        {
            event_desc = event_names[router->lastEventReceived];
        }
    }
    else
    {
        if (router->lastEventReceived <= MAX_EVENT_TYPE)
        {
            event_desc = event_names[router->lastEventReceived];
        }
        else
        {
            /* Check MariaDB 10 new events */
            if (router->lastEventReceived >= MARIADB_NEW_EVENTS_BEGIN
                && router->lastEventReceived <= MAX_EVENT_TYPE_MARIADB10)
            {
                event_desc = event_names_mariadb10[(router->lastEventReceived - MARIADB_NEW_EVENTS_BEGIN)];
            }
        }
    }

    return event_desc;
}

/**
 * Return the event description
 *
 * @param router    The router instance
 * @param event     The current event
 * @return      The event description or NULL
 */
const char* blr_get_event_description(ROUTER_INSTANCE* router, uint8_t event)
{
    const char* event_desc = NULL;

    if (!router->mariadb10_compat)
    {
        if (event <= MAX_EVENT_TYPE)
        {
            event_desc = event_names[event];
        }
    }
    else
    {
        if (event <= MAX_EVENT_TYPE)
        {
            event_desc = event_names[event];
        }
        else
        {
            /* Check MariaDB 10 new events */
            if (event >= MARIADB_NEW_EVENTS_BEGIN
                && event <= MAX_EVENT_TYPE_MARIADB10)
            {
                event_desc = event_names_mariadb10[(event - MARIADB_NEW_EVENTS_BEGIN)];
            }
        }
    }

    return event_desc;
}

/**
 * Free SSL struct in server struct
 *
 * @param inst   The router instance
 */
void blr_free_ssl_data(ROUTER_INSTANCE* inst)
{
    SSL_LISTENER* server_ssl;

    if (inst->service->dbref->server->server_ssl)
    {
        server_ssl = inst->service->dbref->server->server_ssl;

        /*
         * Free SSL struct
         * Note: SSL struct in server should be freed by server_free()
         */
        MXS_FREE(server_ssl->ssl_key);
        MXS_FREE(server_ssl->ssl_ca_cert);
        MXS_FREE(server_ssl->ssl_cert);
        MXS_FREE(inst->service->dbref->server->server_ssl);
        inst->service->dbref->server->server_ssl = NULL;
    }
}

/**
 * destroy binlog server instance
 *
 * @param service   The service this router instance belongs to
 */
static void destroyInstance(MXS_ROUTER* instance)
{
    ROUTER_INSTANCE* inst = (ROUTER_INSTANCE*) instance;

    MXS_DEBUG("Destroying instance of router %s for service %s",
              inst->service->router_name(),
              inst->service->name());

    /* Check whether master connection is active */
    if (inst->master)
    {
        if (inst->master->fd != -1 && inst->master->state == DCB_STATE_POLLING)
        {
            blr_master_close(inst);
        }
    }

    pthread_mutex_lock(&inst->lock);

    if (inst->master_state != BLRM_UNCONFIGURED)
    {
        inst->master_state = BLRM_SLAVE_STOPPED;
    }

    pthread_mutex_unlock(&inst->lock);

    if (inst->client)
    {
        if (inst->client->state == DCB_STATE_POLLING)
        {
            dcb_close(inst->client);
            inst->client = NULL;
        }
    }

    MXS_INFO("%s is being stopped by MaxScale shudown. Disconnecting from master [%s]:%d, "
             "read up to log %s, pos %lu, transaction safe pos %lu",
             inst->service->name(),
             inst->service->dbref->server->address,
             inst->service->dbref->server->port,
             inst->binlog_name,
             inst->current_pos,
             inst->binlog_position);

    if (inst->trx_safe
        && inst->pending_transaction.state > BLRM_NO_TRANSACTION)
    {
        MXS_WARNING("%s stopped by shutdown: detected mid-transaction in binlog file %s, "
                    "pos %lu, incomplete transaction starts at pos %lu",
                    inst->service->name(),
                    inst->binlog_name,
                    inst->current_pos,
                    inst->binlog_position);
    }

    /* Close GTID maps database */
    sqlite3_close_v2(inst->gtid_maps);
}

/**
 * Return the the value from hexadecimal digit
 *
 * @param c    Then hex char
 * @return     The numeric value
 */
unsigned int from_hex(char c)
{
    return c <= '9' ? c - '0' : tolower(c) - 'a' + 10;
}

/**
 * Parse a buffer of HEX data
 *
 * An encryption Key and its len are stored
 * in router->encryption struct
 *
 * @param buffer    A buffer of bytes, in hex format
 * @param nline     The line number in the key file
 * @param router    The router instance
 * @return          true on success and false on error
 */
bool blr_extract_key(const char* buffer, int nline, ROUTER_INSTANCE* router)
{
    char* p = (char*)buffer;
    int length = 0;
    uint8_t* key = (uint8_t*)router->encryption.key_value;

    while (isspace(*p) && *p != '\n')
    {
        p++;
    }

    /* Skip comments */
    if (*p == '#')
    {
        return false;
    }

    unsigned int id = strtoll(p, &p, 10);

    /* key range is 1 .. 255 */
    if (id < 1 || id > 255)
    {
        MXS_WARNING("Invalid Key Id (values 1..255) found in file %s. Line %d, index 0.",
                    router->encryption.key_management_filename,
                    nline);
        return false;
    }

    /* Continue only if read id is BINLOG_SYSTEM_DATA_CRYPTO_SCHEME (value is 1) */
    if (id != BINLOG_SYSTEM_DATA_CRYPTO_SCHEME)
    {
        return false;
    }

    /* Look for ';' separator */
    if (*p != ';')
    {
        MXS_ERROR("Syntax error in Encryption Key file at line %d, index %lu. File %s",
                  nline,
                  p - buffer,
                  router->encryption.key_management_filename);
        return false;
    }

    p++;

    /* Now read the hex data */

    while (isxdigit(p[0]) && isxdigit(p[1]) && length <= BINLOG_AES_MAX_KEY_LEN)
    {
        key[length++] = from_hex(p[0]) * 16 + from_hex(p[1]);
        p += 2;
    }

    if (isxdigit(*p)
        || (length != 16 && length != 24 && length != 32))
    {
        MXS_ERROR("Found invalid Encryption Key at line %d, index %lu. File %s",
                  nline,
                  p - buffer,
                  router->encryption.key_management_filename);
        return false;
    }

    router->encryption.key_len = length;

    return true;
}

/**
 * Read the encryption key form a file
 *
 * The key must be written in HEX format
 *
 * @param router    The router instance
 * @return          false on error and true on success
 */
bool blr_get_encryption_key(ROUTER_INSTANCE* router)
{
    if (router->encryption.key_management_filename == NULL)
    {
        MXS_ERROR("Service %s, encryption key is not set. "
                  "Please specify key filename with 'encryption_key_file'",
                  router->service->name());
        return false;
    }
    else
    {
        int ret;
        memset(router->encryption.key_value, '\0', sizeof(router->encryption.key_value));

        /* Parse key file */
        if (blr_parse_key_file(router) == 0)
        {
            /* Success */
            router->encryption.key_id = BINLOG_SYSTEM_DATA_CRYPTO_SCHEME;
            return true;
        }
    }

    return false;
}

/**
 * Read encryotion key(s) from a file
 *
 * The file could be the MariaDB 10.1 file_key_management_filename
 * where the keys are not encrypted or it could be a file
 * with a single line containing the key id 1
 *
 * @param router    The router instance
 * @return          0 on success (key id 1 found), -1 on errors
 *                  or the number or read lines if key id was not found
 */
int blr_parse_key_file(ROUTER_INSTANCE* router)
{
    char* line = NULL;
    size_t linesize = 0;
    ssize_t linelen;
    bool found_keyid = false;
    int n_lines = 0;
    FILE* file = fopen(router->encryption.key_management_filename, "r");

    if (!file)
    {
        MXS_ERROR("Failed to open KEY file '%s': %s",
                  router->encryption.key_management_filename,
                  mxs_strerror(errno));
        return -1;
    }

    /* Read all lines from the key_file */
    while ((linelen = getline(&line, &linesize, file)) != -1)
    {
        n_lines++;

        /* Parse buffer for key id = 1*/
        if (blr_extract_key(line, n_lines, router))
        {
            router->encryption.key_id = BINLOG_SYSTEM_DATA_CRYPTO_SCHEME;
            found_keyid = true;
            break;
        }
    }

    MXS_FREE(line);

    fclose(file);

    /* Check result */
    if (n_lines == 0)
    {
        MXS_ERROR("KEY file '%s' has no lines.",
                  router->encryption.key_management_filename);
        return -1;
    }

    if (!found_keyid)
    {
        MXS_ERROR("No Key with Id = 1 has been found in file %s. Read %d lines.",
                  router->encryption.key_management_filename,
                  n_lines);
        return n_lines;
    }
    else
    {
        return 0;
    }
}

/**
 * Create / Open R/W GTID maps database
 *
 */
static bool blr_open_gtid_maps_storage(ROUTER_INSTANCE* inst)
{
    char dbpath[PATH_MAX + 1];
    snprintf(dbpath,
             sizeof(dbpath),
             "/%s/%s",
             inst->binlogdir,
             GTID_MAPS_DB);

    /* Open/Create the GTID maps database */
    if (sqlite3_open_v2(dbpath,
                        &inst->gtid_maps,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        NULL) != SQLITE_OK)
    {
        MXS_ERROR("Failed to open GTID maps SQLite database '%s': %s",
                  dbpath,
                  sqlite3_errmsg(inst->gtid_maps));
        return false;
    }

    char* errmsg;
    /* Create the gtid_maps table */
    int rc = sqlite3_exec(inst->gtid_maps,
                          "BEGIN;"
                          "CREATE TABLE IF NOT EXISTS gtid_maps("
                          "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                          "rep_domain INT, "
                          "server_id INT, "
                          "sequence BIGINT, "
                          "binlog_rdir VARCHAR(255), "
                          "binlog_file VARCHAR(255), "
                          "start_pos BIGINT, "
                          "end_pos BIGINT);"
                          "CREATE UNIQUE INDEX IF NOT EXISTS gtid_index "
                          "ON gtid_maps(rep_domain, server_id, sequence, binlog_file);"
                          "COMMIT;",
                          NULL,
                          NULL,
                          &errmsg);
    if (rc != SQLITE_OK)
    {
        MXS_ERROR("Service %s, failed to create GTID index table 'gtid_maps': %s",
                  inst->service->name(),
                  sqlite3_errmsg(inst->gtid_maps));
        sqlite3_free(errmsg);
        /* Close GTID maps database */
        sqlite3_close_v2(inst->gtid_maps);
        return false;
    }

    // If we are opening an existing database, created by a previous version
    // of BLR, the column 'binlog_rdir' is not present, so we add it.
    rc = sqlite3_exec(inst->gtid_maps,
                      "ALTER TABLE gtid_maps ADD COLUMN binlog_rdir VARCHAR(255);",
                      NULL,
                      NULL,
                      &errmsg);

    if (rc != SQLITE_OK)
    {
        std::string s(errmsg);
        sqlite3_free(errmsg);

        if ((rc == SQLITE_ERROR) && (s.find("binlog_rdir") != std::string::npos))
        {
            // If it's the generic error and "binlog_rdir" is in the error message, we
            // assume it is the complaint "duplicate column name: binlog_rdir" and
            // things are fine (the column is already there).
        }
        else
        {
            // Otherwise we bail out.
            MXS_ERROR("Service %s, failed to alter GTID index table 'gtid_map': %s",
                      inst->service->name(),
                      s.c_str());
            sqlite3_close_v2(inst->gtid_maps);
            return false;
        }
    }

    MXS_NOTICE("%s: Service has MariaDB GTID otion set to ON",
               inst->service->name());

    return true;
}

void blr_log_disabled_heartbeat(const ROUTER_INSTANCE* inst)
{
    MXS_WARNING("%s: %s",
                inst->service->name(),
                "MASTER_HEARTBEAT_PERIOD has been set to 0 (disabled): "
                "a master network inactivity will not be handled.");
}
