#include "php.h"

#include "aerospike/aerospike_key.h"
#include "aerospike/as_config.h"
#include "aerospike/as_policy.h"
#include "aerospike/as_status.h"
#include "aerospike/as_scan.h"

#include "aerospike_common.h"
#include "aerospike_policy.h"

#define NESTING_DEPTH_PHP_INI INI_STR("aerospike.nesting_depth") ? atoi(INI_STR("aerospike.nesting_depth")) : 0
#define CONNECT_TIMEOUT_PHP_INI INI_STR("aerospike.connect_timeout") ? (uint32_t) atoi(INI_STR("aerospike.connect_timeout")) : 0
#define READ_TIMEOUT_PHP_INI INI_STR("aerospike.read_timeout") ? (uint32_t) atoi(INI_STR("aerospike.read_timeout")) : 0
#define WRITE_TIMEOUT_PHP_INI INI_STR("aerospike.write_timeout") ? (uint32_t) atoi(INI_STR("aerospike.write_timeout")) : 0
#define LOG_PATH_PHP_INI INI_STR("aerospike.log_path") ? INI_STR("aerospike.log_path") : NULL
#define LOG_LEVEL_PHP_INI INI_STR("aerospike.log_level") ? INI_STR("aerospike.log_level") : NULL
#define SERIALIZER_PHP_INI INI_STR("aerospike.serializer") ? (uint32_t) atoi(INI_STR("aerospike.serializer")) : 0
#define KEY_POLICY_PHP_INI INI_STR("aerospike.key_policy") ? (uint32_t) atoi(INI_STR("aerospike.key_policy")) : 0
#define GEN_POLICY_PHP_INI INI_STR("aerospike.key_gen") ? (uint32_t) atoi(INI_STR("aerospike.key_gen")) : 0

/*
 *******************************************************************************************************
 * MACROS TO BE USED TO SET/IDENTIFY THE ACTUAL OPTIONAL POLICIES PASSED IN BY
 * THE USER. NEEDS TO BE IDENTIFIED TO SET THE REMNANT POLICIES USING INI
 * DEFAULTS.
 * BIT 0 => TIMEOUT
 * BIT 1 => POLICY_KEY
 *
 * Usage:
 * uint8_t options_passed_for_write = 0x0;
 * 
 * Setting appropriate bits:
 *
 * In case options passed by user contains OPT_WRITE_TIMEOUT, set this:
 *      options_passed_for_write |= SET_BIT_OPT_TIMEOUT;
 * In case options passed by user contains OPT_POLICY_KEY, set this:
 *      options_passed_for_write |= SET_BIT_OPT_POLICY_KEY;
 *
 * Identifying the set bits:
 *
 * if (options_passed_for_write & SET_BIT_OPT_TIMEOUT == 0x0) {
 *      // write timeout not set by user; set using default ini value.
 * } else {
 *      // write_timeout is set by user;
 * }
 *******************************************************************************************************
 */
#define SET_BIT_OPT_TIMEOUT 0x00000001
#define SET_BIT_OPT_POLICY_KEY 0x00000010
#define SET_BIT_OPT_POLICY_GEN 0x00000100

/*
 *******************************************************************************************************
 * Function to declare policy constants in Aerospike class.
 *
 * @param Aerospike_ce          The zend class entry for Aerospike class.
 *
 * @return AEROSPIKE_OK if the success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
extern
as_status declare_policy_constants_php(zend_class_entry *Aerospike_ce TSRMLS_DC)
{
    int32_t i;
    as_status   status = AEROSPIKE_OK;

    if (!Aerospike_ce) {
       status = AEROSPIKE_ERR;
       goto exit;
    }

    for (i = 0; i <= AEROSPIKE_CONSTANTS_ARR_SIZE; i++) {
        zend_declare_class_constant_long(
                Aerospike_ce, aerospike_constants[i].constant_str,
                    strlen(aerospike_constants[i].constant_str),
                        aerospike_constants[i].constantno TSRMLS_CC);
    }

exit:
    return status;
}

/*
 *******************************************************************************************************
 * Function for checking and setting the default aerospike policies by reading
 * from php.ini entries if configured by user, else the global defaults.
 * N.B.:
 *      as_config_p, read_policy, write_policy pointers and not checked.
 *      Calling functions should check them.
 *
 * @param as_config_p                   The as_config object to be passed in case of connect.
 * @param read_policy_p                 The as_policy_read to be passed in case of connect/get.
 * @param write_policy_p                The as_policy_write to be passed in case of connect/put.
 * @param operate_policy_p              The as_policy_operate to be passed in case of operations:
 *                                      append, prepend, increment and touch.
 * @param remove_policy_p               The as_policy_remove to be passed in case of remove.
 * @param info_policy_p                 The as_policy_info to be passed in case of
 *                                      scan_info, register, deregister, get_registered,
 *                                      list_registered udfs.
 * @param scan_policy_p                 The as_policy_scan to be passed in case of scan
 *                                      and scanApply.
 * @param query_policy_p                The as_policy_query to be passed in case of
 *                                      as_query_for_each.
 * @param serializer_policy_p           The serialization policy to be passed in case of put.
 * @param batch_policy_p                The batch policy to be passed in case of existsMany() and
 *                                      getMany().
 * @param apply_policy_p                The apply policy to be passed in case of udf_apply.
 * @param options_passed_for_write_p    The actual options if any passed by user in case of put.
 * @param options_passed_for_read_p     The actual options if any passed by user in case of get.
 * @param options_passed_for_operate_p  The actual options if any passed by user in case of operate.
 * @param options_passed_for_remove_p   The actual options if any passed by user in case of remove.
 *
 *******************************************************************************************************
 */
static void
check_and_set_default_policies(as_config *as_config_p,
                               as_policy_read *read_policy_p,
                               as_policy_write *write_policy_p,
                               as_policy_operate *operate_policy_p,
                               as_policy_remove *remove_policy_p,
                               as_policy_info *info_policy_p,
                               as_policy_scan *scan_policy_p,
                               as_policy_query *query_policy_p,
                               uint32_t *serializer_policy_p,
                               as_policy_batch *batch_policy_p,
                               as_policy_apply *apply_policy_p,
                               uint16_t *options_passed_for_write_p,
                               uint8_t *options_passed_for_read_p,
                               uint8_t *options_passed_for_operate_p,
                               uint8_t *options_passed_for_remove_p)
{
    uint32_t ini_value = 0;

    ini_value = READ_TIMEOUT_PHP_INI;
    if (ini_value) {
        if (read_policy_p) {
            if (((NULL != options_passed_for_read_p) &&
                        ((*options_passed_for_read_p & SET_BIT_OPT_TIMEOUT) == 0x0))
                || (options_passed_for_read_p == NULL)) {
                read_policy_p->timeout = ini_value;
            }
            if ((((NULL != options_passed_for_read_p) &&
                            ((*options_passed_for_read_p & SET_BIT_OPT_POLICY_KEY) == 0x0))
                    || (options_passed_for_read_p == NULL))
                    && KEY_POLICY_PHP_INI) {
                read_policy_p->key = KEY_POLICY_PHP_INI;
            }
        }
        if (info_policy_p) {
            info_policy_p->timeout = ini_value;
        }
        if (batch_policy_p) {
            batch_policy_p->timeout = ini_value;
        }
    }
    
    ini_value = WRITE_TIMEOUT_PHP_INI;
    if (ini_value) {
        if (write_policy_p) {
            if (((NULL != options_passed_for_write_p) &&
                        ((*options_passed_for_write_p & SET_BIT_OPT_TIMEOUT) == 0x0))
                    || (options_passed_for_write_p == NULL)) {
                write_policy_p->timeout = ini_value;
            }
            if ((((NULL != options_passed_for_write_p) &&
                            ((*options_passed_for_write_p & SET_BIT_OPT_POLICY_KEY) == 0x0))
                        || (options_passed_for_write_p == NULL))
                        && KEY_POLICY_PHP_INI) {
                write_policy_p->key = KEY_POLICY_PHP_INI;
            }
            if ((((NULL != options_passed_for_write_p) &&
                            ((*options_passed_for_write_p & SET_BIT_OPT_POLICY_GEN) == 0x0))
                         || (options_passed_for_write_p == NULL))
                         && GEN_POLICY_PHP_INI) {
                 write_policy_p->gen = GEN_POLICY_PHP_INI;
            }
        }
        if (operate_policy_p) {
            if (((NULL != options_passed_for_operate_p) &&
                        ((*options_passed_for_operate_p & SET_BIT_OPT_TIMEOUT) == 0x0))
                    || (options_passed_for_operate_p == NULL)) {
                operate_policy_p->timeout = ini_value;
            }
            if ((((NULL != options_passed_for_operate_p) &&
                            ((*options_passed_for_operate_p & SET_BIT_OPT_POLICY_KEY) == 0x0))
                        || (options_passed_for_operate_p == NULL))
                        && KEY_POLICY_PHP_INI) {
                operate_policy_p->key = KEY_POLICY_PHP_INI;
            }
            if ((((NULL != options_passed_for_operate_p) &&
                            ((*options_passed_for_operate_p & SET_BIT_OPT_POLICY_GEN) == 0x0))
                         || (options_passed_for_operate_p == NULL))
                         && GEN_POLICY_PHP_INI) {
                 operate_policy_p->gen = GEN_POLICY_PHP_INI;
            }
        }
        if (remove_policy_p) {
            if (((NULL != options_passed_for_remove_p) &&
                        ((*options_passed_for_remove_p & SET_BIT_OPT_TIMEOUT) == 0x0))
                    || (options_passed_for_remove_p == NULL)) {
                remove_policy_p->timeout = ini_value;
            }
            if ((((NULL != options_passed_for_remove_p) &&
                            ((*options_passed_for_remove_p & SET_BIT_OPT_POLICY_KEY) == 0x0))
                        || (options_passed_for_remove_p == NULL))
                        && KEY_POLICY_PHP_INI) {
                remove_policy_p->key = KEY_POLICY_PHP_INI;
            }
            if ((((NULL != options_passed_for_remove_p) &&
                            ((*options_passed_for_remove_p & SET_BIT_OPT_POLICY_GEN) == 0x0))
                         || (options_passed_for_remove_p == NULL))
                         && GEN_POLICY_PHP_INI) {
                 remove_policy_p->gen = GEN_POLICY_PHP_INI;
            }
        }
        if (info_policy_p) {
            info_policy_p->timeout = ini_value;
        }
        if (scan_policy_p) {
            scan_policy_p->timeout = ini_value;
        }
        if (query_policy_p) {
            query_policy_p->timeout = ini_value;
        }
        if (apply_policy_p) {
            apply_policy_p->timeout = ini_value;
        }
    }

    ini_value = CONNECT_TIMEOUT_PHP_INI;

    if (ini_value && as_config_p) {
        as_config_p->conn_timeout_ms = ini_value;
    }

    ini_value = SERIALIZER_PHP_INI;

    if (ini_value && serializer_policy_p) {
        *serializer_policy_p = ini_value;
    }
}

/*
 *******************************************************************************************************
 * Function for setting the relevant aerospike policies by using the user's
 * optional policy options (if set) else the defaults.
 *
 * @param options_p             The optional parameters.
 * @param generation_value_p    The generation value to be set into put record.
 * @param error_p               The as_error to be populated by the function
 *  *                           with the encountered error if any.
 */
extern void
get_generation_value(zval* options_p, uint16_t* generation_value_p, as_error *error_p TSRMLS_DC)
{
    zval**                  gen_policy_pp = NULL;
    zval**                  gen_value_pp = NULL;

    if (options_p) {
        if (zend_hash_index_find(Z_ARRVAL_P(options_p), OPT_POLICY_GEN, (void **) &gen_policy_pp) == FAILURE) {
            //error_p->code = AEROSPIKE_ERR;
            goto exit;
        }
        if (Z_TYPE_PP(gen_policy_pp) != IS_ARRAY) {
            error_p->code = AEROSPIKE_ERR;
            goto exit;
        }
        zend_hash_index_find(Z_ARRVAL_P(*gen_policy_pp), 1, (void **) &gen_value_pp);

        if (gen_value_pp && (Z_TYPE_PP(gen_value_pp) != IS_LONG)) {
            error_p->code = AEROSPIKE_ERR;
            goto exit;
        }

        if (gen_value_pp) {
            *generation_value_p = Z_LVAL_PP(gen_value_pp);
        }
    } 

exit:
    return;
}



/*
 *******************************************************************************************************
 * Function for setting the relevant aerospike policies by using the user's
 * optional policy options (if set) else the defaults.
 *
 * @param as_config_p           The as_config object to be passed in case of connect.
 * @param read_policy_p         The as_policy_read to be passed in case of connect/get.
 * @param write_policy_p        The as_policy_write to be passed in case of connect/put.
 * @param operate_policy_p      The as_policy_operate to be passed in case of operations:
 *                              append, prepend, increment and touch.
 * @param remove_policy_p       The as_policy_remove to be passed in case of remove.
 * @param info_policy_p         The as_policy_info to be passed in case of
 *                              scan_info, register, deregister, get_registered,
 *                              list_registered udfs.
 * @param scan_policy_p         The as_policy_scan to be passed in case of scan
 *                              and scanApply.
 * @param query_policy_p        The as_policy_query to be passed in case of
 *                              as_query_for_each.
 * @param serializer_policy_p   The serialization policy to be passed in case of put.
 * @param batch_policy_p        The as_policy_batch to be passed in case of getMany
 *                              and existsMany.
 * @param options_p             The user's optional policy options to be used if set, else defaults.
 * @param error_p               The as_error to be populated by the function
 *                              with the encountered error if any.
 *
 *******************************************************************************************************
 */
static void
set_policy_ex(as_config *as_config_p,
              as_policy_read *read_policy_p,
              as_policy_write *write_policy_p,
              as_policy_operate *operate_policy_p,
              as_policy_remove *remove_policy_p,
              as_policy_info *info_policy_p,
              as_policy_scan *scan_policy_p,
              as_policy_query *query_policy_p,
              uint32_t *serializer_policy_p,
              as_scan* as_scan_p,
              as_policy_batch *batch_policy_p,
              as_policy_apply *apply_policy_p,
              zval *options_p,
              as_error *error_p TSRMLS_DC)
{
    if ((!read_policy_p) && (!write_policy_p) &&
        (!operate_policy_p) && (!remove_policy_p) && (!info_policy_p) &&
        (!scan_policy_p) && (!query_policy_p) && (!serializer_policy_p)
        && (!batch_policy_p) && (!apply_policy_p)) {
        DEBUG_PHP_EXT_DEBUG("Unable to set policy");
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR, "Unable to set policy");
        goto exit;
    }

    /*
     * case: connect => (read_policy_p != NULL && write_policy_p != NULL)
     */

    if (read_policy_p && (!write_policy_p)) {
        /*
         * case: get
         */
        as_policy_read_init(read_policy_p);
    } else if (write_policy_p && (!read_policy_p)) {
        /*
         * case: put
         */
        as_policy_write_init(write_policy_p);
    } else if (operate_policy_p) {
        /*
         * case: operate
         */
        as_policy_operate_init(operate_policy_p);
    } else if (remove_policy_p) {
        /*
         * case: remove
         */
        as_policy_remove_init(remove_policy_p);
    } else if (info_policy_p) {
        /*
         * case: info
         */
        as_policy_info_init(info_policy_p);
    } else if (scan_policy_p) {
        /*
         * case: scan, scanApply
         */
        as_policy_scan_init(scan_policy_p);
    } else if (query_policy_p) {
        /*
         * case: query, aggregate
         */
        as_policy_query_init(query_policy_p);
    } else if (batch_policy_p) {
        /*
         * case: getMany, existsMany
         */
        as_policy_batch_init(batch_policy_p);
    } else if(apply_policy_p) {
        /*
         * case : apply udf
         */
        as_policy_apply_init(apply_policy_p);
    }

    if (options_p == NULL) {
        check_and_set_default_policies(as_config_p, read_policy_p,
                       write_policy_p, operate_policy_p, remove_policy_p,
                       info_policy_p, scan_policy_p, query_policy_p,
                       serializer_policy_p, batch_policy_p, apply_policy_p,
                       NULL, NULL, NULL, NULL);
    } else {
        HashTable*          options_array = Z_ARRVAL_P(options_p);
        HashPosition        options_pointer;
        zval**              options_value;
        int8_t*             options_key;
        int16_t             connect_flag = 0;
        int16_t             serializer_flag = 0;
        int	                scan_percentage = 0;
        uint16_t            options_passed_for_write = 0x0;
        uint8_t             options_passed_for_read = 0x0;
        uint8_t             options_passed_for_operate = 0x0;
        uint8_t             options_passed_for_remove = 0x0;
        zval**              gen_policy_pp = NULL;

        foreach_hashtable(options_array, options_pointer, options_value) {
            uint options_key_len;
            ulong options_index;

            if (zend_hash_get_current_key_ex(options_array, (char **) &options_key, 
                        &options_key_len, &options_index, 0, &options_pointer)
                                != HASH_KEY_IS_LONG) {
                DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Policy Constant Key");
                PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                        "Unable to set policy: Invalid Policy Constant Key");
                goto exit;
            }
            switch((int) options_index) {
                case OPT_CONNECT_TIMEOUT:
                    if ((!as_config_p) || (Z_TYPE_PP(options_value) != IS_LONG)) {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_CONNECT_TIMEOUT");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_CONNECT_TIMEOUT");
                        goto exit;
                    }
                    as_config_p->conn_timeout_ms = (uint32_t) Z_LVAL_PP(options_value);
                    connect_flag = 1;
                    break;
                case OPT_READ_TIMEOUT:
                    if (Z_TYPE_PP(options_value) != IS_LONG) {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_READ_TIMEOUT");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_READ_TIMEOUT");
                        goto exit;
                    }
                    if (read_policy_p) {
                        read_policy_p->timeout = (uint32_t) Z_LVAL_PP(options_value);
                    } else if (info_policy_p) {
                        info_policy_p->timeout = (uint32_t) Z_LVAL_PP(options_value);
                    } else if (scan_policy_p) {
                        scan_policy_p->timeout = (uint32_t) Z_LVAL_PP(options_value);
                    } else if (query_policy_p) {
                        query_policy_p->timeout = (uint32_t) Z_LVAL_PP(options_value);
                    } else if (batch_policy_p) {
                        batch_policy_p->timeout = (uint32_t) Z_LVAL_PP(options_value);
                    } else {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_READ_TIMEOUT");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_READ_TIMEOUT");
                        goto exit;
                    }
                    options_passed_for_read |= SET_BIT_OPT_TIMEOUT;
                    break;
                case OPT_WRITE_TIMEOUT:
                    if (Z_TYPE_PP(options_value) != IS_LONG) {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_WRITE_TIMEOUT");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_WRITE_TIMEOUT");
                        goto exit;
                    }
                    if (write_policy_p) {
                        write_policy_p->timeout = (uint32_t) Z_LVAL_PP(options_value);
                    } else if(operate_policy_p) {
                        operate_policy_p->timeout = (uint32_t) Z_LVAL_PP(options_value);
                        options_passed_for_operate |= SET_BIT_OPT_TIMEOUT;
                    } else if(remove_policy_p) {
                        remove_policy_p->timeout = (uint32_t) Z_LVAL_PP(options_value);
                        options_passed_for_remove |= SET_BIT_OPT_TIMEOUT;
                    } else if(info_policy_p) {
                        info_policy_p->timeout = (uint32_t) Z_LVAL_PP(options_value);
                    } else if(scan_policy_p) {
                        scan_policy_p->timeout = (uint32_t) Z_LVAL_PP(options_value);
                    } else if(query_policy_p) {
                        query_policy_p->timeout = (uint32_t) Z_LVAL_PP(options_value);
                    } else if (apply_policy_p) {
                        apply_policy_p->timeout = (uint32_t) Z_LVAL_PP(options_value);
                    } else {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_WRITE_TIMEOUT");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_WRITE_TIMEOUT");
                        goto exit;
                    }
                    options_passed_for_write |= SET_BIT_OPT_TIMEOUT;
                    break;
                case OPT_POLICY_EXISTS:
                    if ((!write_policy_p) || (Z_TYPE_PP(options_value) != IS_LONG)) {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_EXISTS");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_POLICY_EXISTS");
                        goto exit;
                    }
                    write_policy_p->exists = Z_LVAL_PP(options_value);
                    break;
                case OPT_POLICY_RETRY:
                    if (Z_TYPE_PP(options_value) != IS_LONG) {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_RETRY");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_POLICY_RETRY");
                        goto exit;
                    }
                    if (write_policy_p) {
                        write_policy_p->retry = Z_LVAL_PP(options_value);
                    } else if(operate_policy_p) {
                        operate_policy_p->retry = Z_LVAL_PP(options_value);
                    } else if(remove_policy_p) {
                        remove_policy_p->retry = Z_LVAL_PP(options_value);
                    } else {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_RETRY");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_POLICY_RETRY");
                        goto exit;
                    }
                    break;
                case OPT_SERIALIZER:
                    if ((!serializer_policy_p) || (Z_TYPE_PP(options_value) != IS_LONG)) {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_RETRY");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_POLICY_RETRY");
                        goto exit;
                    }
                    *serializer_policy_p = Z_LVAL_PP(options_value);
                    serializer_flag = 1;
                    break;
                case OPT_SCAN_PRIORITY:
                    if (info_policy_p) {
                        break;
                    }
                    if ((!as_scan_p) || (Z_TYPE_PP(options_value) != IS_LONG)) {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_SCAN_PRIORITY");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_SCAN_PRIORITY");
                        goto exit;
                    }
                    if (!as_scan_set_priority(as_scan_p, (uint32_t) Z_LVAL_PP(options_value))) {
                        DEBUG_PHP_EXT_DEBUG("Unable to set scan priority");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR, "Unable to set scan priority");
                        goto exit;
                    }
                    break;
                case OPT_SCAN_PERCENTAGE:
                    if (info_policy_p) {
                        break;
                    }
                    if ((!as_scan_p) || (Z_TYPE_PP(options_value) != IS_LONG)) {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_SCAN_PERCENTAGE");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_SCAN_PERCENTAGE");
                        goto exit;
                    }
                    scan_percentage = Z_LVAL_PP(options_value);
                    if (scan_percentage < 0 || scan_percentage > 100) {
                        DEBUG_PHP_EXT_DEBUG("Invalid value for scan percent");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR, "Invalid value for scan percent");
                        goto exit;
                    } else if (!as_scan_set_percent(as_scan_p, scan_percentage)) {
                        DEBUG_PHP_EXT_DEBUG("Unable to set scan percent");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR, "Unable to set scan percent");
                        goto exit;
                    }
                    break;
                case OPT_SCAN_CONCURRENTLY:
                    if (info_policy_p) {
                        break;
                    }
                    if ((!as_scan_p) || (Z_TYPE_PP(options_value) != IS_BOOL)) {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_READ_TIMEOUT");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_READ_TIMEOUT");
                        goto exit;
                    }
                    if (!as_scan_set_concurrent(as_scan_p, (uint32_t) Z_BVAL_PP(options_value))) {
                        DEBUG_PHP_EXT_DEBUG("Unable to set scan concurrency");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR, "Unable to set scan concurrency");
                        goto exit;
                    }
                    break;
                case OPT_SCAN_NOBINS:
                    if (info_policy_p) {
                        break;
                    }
                    if ((!as_scan_p) || (Z_TYPE_PP(options_value) != IS_BOOL)) {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_READ_TIMEOUT");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_READ_TIMEOUT");
                        goto exit;
                    }
                    if (!as_scan_set_nobins(as_scan_p, (uint32_t) Z_BVAL_PP(options_value))) {
                        DEBUG_PHP_EXT_DEBUG("Unable to set scan no bins");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR, "Unable to set scan no bins");
                        goto exit;
                    }
                    break;
                case OPT_POLICY_KEY:
                    if (Z_TYPE_PP(options_value) != IS_LONG) {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_KEY");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_POLICY_KEY");
                        goto exit;
                    }
                    if (write_policy_p) {
                        write_policy_p->key = Z_LVAL_PP(options_value);
                        options_passed_for_write |= SET_BIT_OPT_POLICY_KEY;
                    } else if (read_policy_p) {
                        read_policy_p->key = Z_LVAL_PP(options_value);
                        options_passed_for_read |= SET_BIT_OPT_POLICY_KEY;
                    } else if (operate_policy_p) {
                        operate_policy_p->key = Z_LVAL_PP(options_value);
                        options_passed_for_operate |= SET_BIT_OPT_POLICY_KEY;
                    } else if (remove_policy_p) {
                        remove_policy_p->key = Z_LVAL_PP(options_value);
                        options_passed_for_remove |= SET_BIT_OPT_POLICY_KEY;
                    } else {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_KEY");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_POLICY_KEY");
                        goto exit;
                    }
                    break;
                case OPT_POLICY_GEN:
                    zend_hash_index_find(Z_ARRVAL_P(*options_value), 0, (void **) &gen_policy_pp);

                    if (Z_TYPE_PP(gen_policy_pp) != IS_LONG) {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_GEN");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_POLICY_GEN");
                        goto exit;
                    }
                    if (write_policy_p) {
                        write_policy_p->gen = Z_LVAL_PP(gen_policy_pp);
                        options_passed_for_write |= SET_BIT_OPT_POLICY_GEN;
                    } else if (operate_policy_p) {
                        operate_policy_p->gen = Z_LVAL_PP(gen_policy_pp);
                        options_passed_for_operate |= SET_BIT_OPT_POLICY_GEN;
                    } else if (remove_policy_p) {
                        remove_policy_p->gen = Z_LVAL_PP(gen_policy_pp);
                        options_passed_for_remove |= SET_BIT_OPT_POLICY_GEN;
                    } else {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_GEN");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_POLICY_GEN");
                        goto exit;
                    }
                    break;
                case OPT_POLICY_COMMIT_LEVEL:
                    if (Z_TYPE_PP(options_value) != IS_LONG) {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_COMMIT_LEVEL");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_POLICY_COMMIT_LEVEL");
                        goto exit;
                    }
                    if (write_policy_p) {
                        write_policy_p->commit_level = Z_LVAL_PP(options_value);
                    } else if (operate_policy_p) {
                        operate_policy_p->commit_level = Z_LVAL_PP(options_value);
                    } else if (remove_policy_p) {
                        remove_policy_p->commit_level = Z_LVAL_PP(options_value);
                    } else {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_COMMIT_LEVEL");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_POLICY_COMMIT_LEVEL");
                        goto exit;
                    }
                    break;
                case OPT_POLICY_CONSISTENCY:
                    if (Z_TYPE_PP(options_value) != IS_LONG) {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_CONSISTENCY");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_POLICY_CONSISTENCY");
                        goto exit;
                    }
                    if (read_policy_p) {
                        read_policy_p->consistency_level = Z_LVAL_PP(options_value);
                    } else if (operate_policy_p) {
                        operate_policy_p->consistency_level = Z_LVAL_PP(options_value);
                    } else {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_CONSISTENCY");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_POLICY_CONSISTENCY");
                        goto exit;
                    }
                    break;
                case OPT_POLICY_REPLICA:
                    if (Z_TYPE_PP(options_value) != IS_LONG) {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_REPLICA");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_POLICY_REPLICA");
                        goto exit;
                    }
                    if (read_policy_p) {
                        read_policy_p->replica = Z_LVAL_PP(options_value);
                    } else if (operate_policy_p) {
                        operate_policy_p->replica = Z_LVAL_PP(options_value);
                    } else {
                        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_REPLICA");
                        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                                "Unable to set policy: Invalid Value for OPT_POLICY_REPLICA");
                        goto exit;
                    }
                    break;
                default:
                    DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Policy Constant Key");
                    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                            "Unable to set policy: Invalid Policy Constant Key");
                    goto exit;
            }
        }
        if (write_policy_p) {
            check_and_set_default_policies((connect_flag ? NULL : as_config_p),
                    NULL, write_policy_p, NULL, NULL, NULL, NULL, NULL, NULL,
                    NULL, NULL, &options_passed_for_write, NULL, NULL, NULL);
            connect_flag = 1;
        }
        if (read_policy_p) {
            check_and_set_default_policies((connect_flag ? NULL : as_config_p),
                    read_policy_p, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                    NULL, NULL, NULL, &options_passed_for_read, NULL, NULL);
            connect_flag = 1;
        }
        if (operate_policy_p) {
            check_and_set_default_policies(NULL, NULL, NULL, operate_policy_p,
                    NULL, NULL, NULL, NULL, NULL,
                    NULL, NULL, NULL, NULL, &options_passed_for_operate, NULL);
        }
        if (remove_policy_p) {
            check_and_set_default_policies(NULL, NULL, NULL, NULL,
                    remove_policy_p, NULL, NULL, NULL, NULL,
                    NULL, NULL, NULL, NULL, NULL, &options_passed_for_remove);
        }
        if (!connect_flag && as_config_p) {
            check_and_set_default_policies(as_config_p, NULL, NULL, NULL,
                    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
        }
        if (!serializer_flag && serializer_policy_p) {
            check_and_set_default_policies(NULL, NULL, NULL, NULL, NULL, NULL,
                    NULL, NULL, serializer_policy_p, NULL, NULL, NULL, NULL, NULL, NULL);
        }
    }
    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);
exit:
    return;
}

/*
 *******************************************************************************************************
 * Wrapper function for setting the relevant aerospike policies by using the user's
 * optional policy options (if set) else the defaults.
 * (Called by all methods except connect.)
 *
 * @param read_policy_p         The as_policy_read to be passed in case of get.
 * @param write_policy_p        The as_policy_write to be passed in case of put.
 * @param operate_policy_p      The as_policy_operate to be passed in case of operations:
 *                              append, prepend, increment and touch.
 * @param remove_policy_p       The as_policy_remove to be passed in case of remove.
 * @param info_policy_p         The as_policy_info to be passed in case of
 *                              scan_info, register, deregister, get_registered,
 *                              list_registered udfs.
 * @param scan_policy_p         The as_policy_scan to be passed in case of scan
 *                              and scanApply.
 * @param query_policy_p        The as_policy_query to be passed in case of
 *                              as_query_for_each.
 * @param serializer_policy_p   The serialization policy to be passed in case of put.
 * @param options_p             The user's optional policy options to be used if set, else defaults.
 * @param error_p               The as_error to be populated by the function
 *                              with the encountered error if any.
 *
 *******************************************************************************************************
 */
extern void
set_policy(as_policy_read *read_policy_p,
           as_policy_write *write_policy_p,
           as_policy_operate *operate_policy_p,
           as_policy_remove *remove_policy_p,
           as_policy_info *info_policy_p,
           as_policy_scan *scan_policy_p,
           as_policy_query *query_policy_p,
           uint32_t *serializer_policy_p,
           zval *options_p,
           as_error *error_p TSRMLS_DC)
{
    set_policy_ex(NULL, read_policy_p, write_policy_p, operate_policy_p,
            remove_policy_p, info_policy_p, scan_policy_p, query_policy_p,
            serializer_policy_p, NULL, NULL, NULL, options_p, error_p TSRMLS_CC);
}

extern void
set_policy_scan(as_policy_scan *scan_policy_p,
        uint32_t *serializer_policy_p,
        as_scan *as_scan_p,
        zval *options_p,
        as_error *error_p TSRMLS_DC)
{
    set_policy_ex(NULL, NULL, NULL, NULL, NULL, NULL, scan_policy_p, NULL,
            serializer_policy_p, as_scan_p, NULL, NULL, options_p, error_p TSRMLS_CC);
}

extern void
set_policy_batch(as_policy_batch *batch_policy_p,
        zval *options_p,
        as_error *error_p TSRMLS_DC)
{
    set_policy_ex(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
            NULL, NULL, batch_policy_p, NULL, options_p, error_p TSRMLS_CC);
}

extern void
set_policy_udf_apply(as_policy_apply *apply_policy_p,
        zval *options_p,
        as_error *error_p TSRMLS_DC)
{
    set_policy_ex(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, apply_policy_p, options_p, error_p TSRMLS_CC);
}
/*
 *******************************************************************************************************
 * Wrapper function for setting the relevant aerospike policies by using the user's
 * optional policy options (if set) else the defaults.
 * (Called in case of connect.)
 *
 * @param as_config_p           The as_config object to be passed in case of connect.
 * @param options_p             The user's optional policy options to be used if set, else defaults.
 * @param error_p               The as_error to be populated by the function
 *                              with the encountered error if any.
 *
 *******************************************************************************************************
 */
extern void
set_general_policies(as_config *as_config_p,
                     zval *options_p,
                     as_error *error_p TSRMLS_DC)
{
    if (!as_config_p) {
        DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid as_config");
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR, "Unable to set policy: Invalid as_config");
        goto exit;
    }

    set_policy_ex(as_config_p, &as_config_p->policies.read, &as_config_p->policies.write,
                  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, options_p, error_p TSRMLS_CC);
exit:
    return;
}
