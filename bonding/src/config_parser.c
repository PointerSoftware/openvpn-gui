/**
 * @file config_parser.c
 * @brief Configuration file parser implementation
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include <time.h>
#include "bonding_config.h"

/* Forward declarations for helper functions */
static int parse_line(char *line, char **key, char **value);
static int parse_global_parameter(bonding_profile_t *profile, const char *key, const char *value);
static int parse_tunnel_parameter(tunnel_config_t *tunnel, const char *key, const char *value);
static int parse_section_header(const char *line, int *tunnel_index);
static int is_comment_line(const char *line);
static int is_empty_line(const char *line);
static char* trim_whitespace(char *str);
static int parse_integer(const char *str, int *value);
static char* duplicate_string(const char *str);
static WCHAR* duplicate_wstring(const WCHAR *str);

/* External functions from main.h and misc.h - declared as extern for linking */
extern void MsgToEventLog(WORD type, wchar_t *format, ...);
extern WCHAR* Widen(const char *utf8);
extern char* WCharToUTF8(const WCHAR *wstr);
extern BOOL CheckFileAccess(const TCHAR *path, int access);

#ifdef DEBUG
extern void PrintDebug(TCHAR *format, ...);
#else
#define PrintDebug(...) do {} while(0)
#endif

/**
 * @brief Convert bonding mode string to enum
 * Returns BONDING_MODE_ROUND_ROBIN for invalid/unknown modes (default)
 */
bonding_mode_t bonding_mode_from_string(const char *mode_str)
{
    if (!mode_str)
        return BONDING_MODE_ROUND_ROBIN;

    if (strcmp(mode_str, "round-robin") == 0)
        return BONDING_MODE_ROUND_ROBIN;
    else if (strcmp(mode_str, "weighted") == 0)
        return BONDING_MODE_WEIGHTED;
    else if (strcmp(mode_str, "active-backup") == 0)
        return BONDING_MODE_ACTIVE_BACKUP;
    else if (strcmp(mode_str, "adaptive") == 0)
        return BONDING_MODE_ADAPTIVE;
    else
        return BONDING_MODE_ROUND_ROBIN; /* Default for invalid input */
}

/**
 * @brief Convert bonding mode enum to string
 */
const char* bonding_mode_to_string(bonding_mode_t mode)
{
    switch (mode) {
        case BONDING_MODE_ROUND_ROBIN:
            return "round-robin";
        case BONDING_MODE_WEIGHTED:
            return "weighted";
        case BONDING_MODE_ACTIVE_BACKUP:
            return "active-backup";
        case BONDING_MODE_ADAPTIVE:
            return "adaptive";
        default:
            return "round-robin";
    }
}

/**
 * @brief Check if line is a comment
 */
static int is_comment_line(const char *line)
{
    if (!line)
        return 0;
    
    /* Skip leading whitespace */
    while (isspace((unsigned char)*line))
        line++;
    
    return (*line == '#' || *line == ';');
}

/**
 * @brief Check if line is empty (only whitespace)
 */
static int is_empty_line(const char *line)
{
    if (!line)
        return 1;
    
    while (*line) {
        if (!isspace((unsigned char)*line))
            return 0;
        line++;
    }
    return 1;
}

/**
 * @brief Trim leading and trailing whitespace
 */
static char* trim_whitespace(char *str)
{
    char *end;
    
    if (!str)
        return NULL;
    
    /* Trim leading whitespace */
    while (isspace((unsigned char)*str))
        str++;
    
    if (*str == 0)
        return str;
    
    /* Trim trailing whitespace */
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
        end--;
    
    end[1] = '\0';
    return str;
}

/**
 * @brief Parse integer from string
 */
static int parse_integer(const char *str, int *value)
{
    char *endptr;
    long int val;
    
    if (!str || !value)
        return -1;
    
    val = strtol(str, &endptr, 10);
    if (*endptr != '\0' && !isspace((unsigned char)*endptr))
        return -1; /* Invalid character */
    
    if (val < INT_MIN || val > INT_MAX)
        return -1; /* Out of range */
    
    *value = (int)val;
    return 0;
}

/**
 * @brief Duplicate a string with error handling
 */
static char* duplicate_string(const char *str)
{
    char *dup;
    size_t len;
    
    if (!str)
        return NULL;
    
    len = strlen(str) + 1;
    dup = (char*)calloc(len, 1);
    if (!dup)
        return NULL;
    
    memcpy(dup, str, len - 1);
    return dup;
}

/**
 * @brief Duplicate a wide string with error handling
 */
static WCHAR* duplicate_wstring(const WCHAR *str)
{
    WCHAR *dup;
    size_t len;
    
    if (!str)
        return NULL;
    
    len = wcslen(str) + 1;
    dup = (WCHAR*)calloc(len, sizeof(WCHAR));
    if (!dup)
        return NULL;
    
    wcscpy(dup, str);
    return dup;
}

/**
 * @brief Parse a line into key-value pair
 * Supports both key=value and space-delimited key value syntax
 */
static int parse_line(char *line, char **key, char **value)
{
    char *equals;
    char *key_start, *value_start;
    char *space_sep;
    
    if (!line || !key || !value)
        return -1;
    
    *key = NULL;
    *value = NULL;
    
    /* Try to find equals sign first (key=value syntax) */
    equals = strchr(line, '=');
    if (equals) {
        /* Extract key */
        key_start = line;
        while (isspace((unsigned char)*key_start))
            key_start++;
        
        *equals = '\0';
        *key = trim_whitespace(key_start);
        
        /* Extract value */
        value_start = equals + 1;
        while (isspace((unsigned char)*value_start))
            value_start++;
        
        /* Remove quotes if present */
        if (*value_start == '"') {
            value_start++;
            if (value_start[strlen(value_start) - 1] == '"')
                value_start[strlen(value_start) - 1] = '\0';
        }
        
        *value = trim_whitespace(value_start);
        
        return 0;
    }
    
    /* Try space-delimited syntax (key value) */
    key_start = line;
    while (isspace((unsigned char)*key_start))
        key_start++;
    
    if (*key_start == '\0')
        return -1; /* Empty line */
    
    /* Find first space after key */
    space_sep = key_start;
    while (*space_sep && !isspace((unsigned char)*space_sep))
        space_sep++;
    
    if (*space_sep == '\0')
        return -1; /* No value found */
    
    /* Terminate key */
    *space_sep = '\0';
    *key = trim_whitespace(key_start);
    
    /* Extract value */
    value_start = space_sep + 1;
    while (isspace((unsigned char)*value_start))
        value_start++;
    
    if (*value_start == '\0')
        return -1; /* No value after space */
    
    /* Remove quotes if present */
    if (*value_start == '"') {
        value_start++;
        if (value_start[strlen(value_start) - 1] == '"')
            value_start[strlen(value_start) - 1] = '\0';
    }
    
    *value = trim_whitespace(value_start);
    
    return 0;
}

/**
 * @brief Parse section header [tunnel-N]
 */
static int parse_section_header(const char *line, int *tunnel_index)
{
    const char *start, *end;
    char num_str[16];
    int num;
    
    if (!line || !tunnel_index)
        return -1;
    
    /* Find [tunnel- */
    start = strstr(line, "[tunnel-");
    if (!start)
        return -1;
    
    start += 8; /* Skip "[tunnel-" */
    
    /* Find closing bracket */
    end = strchr(start, ']');
    if (!end)
        return -1;
    
    /* Extract number */
    if ((size_t)(end - start) >= sizeof(num_str))
        return -1;
    
    memcpy(num_str, start, end - start);
    num_str[end - start] = '\0';
    
    if (parse_integer(num_str, &num) != 0)
        return -1;
    
    if (num < 0 || num >= MAX_TUNNEL_COUNT)
        return -1;
    
    *tunnel_index = num;
    return 0;
}

/**
 * @brief Parse global parameter
 */
static int parse_global_parameter(bonding_profile_t *profile, const char *key, const char *value)
{
    if (!profile || !key || !value)
        return -1;
    
    if (strcmp(key, "bonding-mode") == 0) {
        bonding_mode_t parsed_mode = bonding_mode_from_string(value);
        /* Check if an invalid mode was provided (function returns round-robin for invalid input) */
        if (parsed_mode == BONDING_MODE_ROUND_ROBIN && 
            strcmp(value, "round-robin") != 0 &&
            strcmp(value, "weighted") != 0 &&
            strcmp(value, "active-backup") != 0 &&
            strcmp(value, "adaptive") != 0) {
            /* Invalid mode - parsed to default but input was not a valid mode string */
            return -1;
        }
        profile->mode = parsed_mode;
    } else if (strcmp(key, "tunnel-count") == 0) {
        if (parse_integer(value, &profile->tunnel_count) != 0)
            return -1;
        
        if (profile->tunnel_count <= 0 || profile->tunnel_count > MAX_TUNNEL_COUNT)
            return -1;
        
        /* Allocate tunnel array */
        if (profile->tunnels == NULL) {
            profile->tunnels = (tunnel_config_t*)calloc(profile->tunnel_count, sizeof(tunnel_config_t));
            if (!profile->tunnels)
                return -1;
        }
    } else if (strncmp(key, "profile", 7) == 0 || strncmp(key, "Profile", 7) == 0) {
        /* Profile name from comment - extract if needed */
        if (!profile->profile_name) {
            profile->profile_name = duplicate_string(value);
        }
    } else if (strcmp(key, "packet-queue-size") == 0) {
        int queue_size;
        if (parse_integer(value, &queue_size) != 0)
            return -1;
        if (queue_size < 100 || queue_size > 10000)
            return -1;
        profile->packet_queue_size = queue_size;
    } else if (strcmp(key, "packet-timeout") == 0) {
        int timeout;
        if (parse_integer(value, &timeout) != 0)
            return -1;
        if (timeout <= 0)
            return -1;
        profile->packet_timeout = timeout;
    } else if (strcmp(key, "sequencing-enabled") == 0) {
        /* Boolean value: "yes", "no", "1", "0", "true", "false" */
        int enabled = 0;
        if (strcmp(value, "yes") == 0 || strcmp(value, "1") == 0 || strcmp(value, "true") == 0) {
            enabled = 1;
        } else if (strcmp(value, "no") == 0 || strcmp(value, "0") == 0 || strcmp(value, "false") == 0) {
            enabled = 0;
        } else {
            return -1; /* Invalid value */
        }
        profile->sequencing_enabled = enabled;
    } else if (strcmp(key, "flow-control-enabled") == 0) {
        /* Boolean value: "yes", "no", "1", "0", "true", "false" */
        int enabled = 0;
        if (strcmp(value, "yes") == 0 || strcmp(value, "1") == 0 || strcmp(value, "true") == 0) {
            enabled = 1;
        } else if (strcmp(value, "no") == 0 || strcmp(value, "0") == 0 || strcmp(value, "false") == 0) {
            enabled = 0;
        } else {
            return -1; /* Invalid value */
        }
        profile->flow_control_enabled = enabled;
    }
    
    return 0;
}

/**
 * @brief Parse tunnel parameter
 */
static int parse_tunnel_parameter(tunnel_config_t *tunnel, const char *key, const char *value)
{
    int port, weight;
    
    if (!tunnel || !key || !value)
        return -1;
    
    if (strcmp(key, "nic-name") == 0) {
        if (tunnel->nic_name)
            free(tunnel->nic_name);
        tunnel->nic_name = duplicate_string(value);
        if (!tunnel->nic_name)
            return -1;
    } else if (strcmp(key, "server-host") == 0) {
        if (tunnel->server_host)
            free(tunnel->server_host);
        tunnel->server_host = duplicate_string(value);
        if (!tunnel->server_host)
            return -1;
    } else if (strcmp(key, "server-port") == 0) {
        if (parse_integer(value, &port) != 0)
            return -1;
        if (port < 1 || port > 65535)
            return -1;
        tunnel->server_port = port;
    } else if (strcmp(key, "server-config") == 0) {
        if (tunnel->config_file)
            free(tunnel->config_file);
        tunnel->config_file = duplicate_string(value);
        if (!tunnel->config_file)
            return -1;
    } else if (strcmp(key, "weight") == 0) {
        if (parse_integer(value, &weight) != 0)
            return -1;
        if (weight <= 0)
            return -1;
        tunnel->weight = weight;
    }
    
    return 0;
}

/**
 * @brief Validate configuration
 */
validation_result_t bonding_config_validate(bonding_profile_t *profile)
{
    validation_result_t result = {0};
    int i, j;
    WCHAR *wpath;
    
    if (!profile) {
        result.error_code = BONDING_ERROR_VALIDATION_ERROR;
        strcpy(result.error_message, "Profile is NULL");
        return result;
    }
    
    /* Check bonding mode */
    if (profile->mode != BONDING_MODE_ROUND_ROBIN &&
        profile->mode != BONDING_MODE_WEIGHTED &&
        profile->mode != BONDING_MODE_ACTIVE_BACKUP &&
        profile->mode != BONDING_MODE_ADAPTIVE) {
        result.error_code = BONDING_ERROR_INVALID_MODE;
        strcpy(result.error_message, "Invalid bonding mode");
        return result;
    }
    
    /* Validate packet distributor configuration */
    if (profile->packet_queue_size < 100 || profile->packet_queue_size > 10000) {
        result.error_code = BONDING_ERROR_VALIDATION_ERROR;
        snprintf(result.error_message, sizeof(result.error_message),
                 "Invalid packet-queue-size: %d (must be 100-10000)", profile->packet_queue_size);
        return result;
    }
    
    if (profile->packet_timeout <= 0) {
        result.error_code = BONDING_ERROR_VALIDATION_ERROR;
        snprintf(result.error_message, sizeof(result.error_message),
                 "Invalid packet-timeout: %d (must be > 0)", profile->packet_timeout);
        return result;
    }
    
    if (profile->sequencing_enabled != 0 && profile->sequencing_enabled != 1) {
        result.error_code = BONDING_ERROR_VALIDATION_ERROR;
        snprintf(result.error_message, sizeof(result.error_message),
                 "Invalid sequencing-enabled: %d (must be 0 or 1)", profile->sequencing_enabled);
        return result;
    }
    
    if (profile->flow_control_enabled != 0 && profile->flow_control_enabled != 1) {
        result.error_code = BONDING_ERROR_VALIDATION_ERROR;
        snprintf(result.error_message, sizeof(result.error_message),
                 "Invalid flow-control-enabled: %d (must be 0 or 1)", profile->flow_control_enabled);
        return result;
    }
    
    /* Check tunnel count */
    if (profile->tunnel_count <= 0 || profile->tunnel_count > MAX_TUNNEL_COUNT) {
        result.error_code = BONDING_ERROR_INVALID_TUNNEL_COUNT;
        snprintf(result.error_message, sizeof(result.error_message),
                 "Invalid tunnel count: %d (must be 1-%d)", profile->tunnel_count, MAX_TUNNEL_COUNT);
        return result;
    }
    
    if (!profile->tunnels) {
        result.error_code = BONDING_ERROR_MISSING_PARAMETER;
        strcpy(result.error_message, "Tunnel array not allocated");
        return result;
    }
    
    /* Validate tunnel-count matches actual parsed tunnel sections (only if tunnel_count > 0) */
    if (profile->tunnel_count > 0 && profile->parsed_tunnel_count != profile->tunnel_count) {
        result.error_code = BONDING_ERROR_INVALID_TUNNEL_COUNT;
        snprintf(result.error_message, sizeof(result.error_message),
                 "Tunnel count mismatch: declared %d but found %d [tunnel-N] sections",
                 profile->tunnel_count, profile->parsed_tunnel_count);
        return result;
    }
    
    /* Validate tunnel indices are sequential starting from 0 */
    if (profile->parsed_tunnel_count > 0) {
        for (i = 0; i < profile->parsed_tunnel_count; i++) {
            if (profile->parsed_tunnel_indices[i] != i) {
                result.error_code = BONDING_ERROR_INVALID_TUNNEL_COUNT;
                snprintf(result.error_message, sizeof(result.error_message),
                         "Non-sequential tunnel indices: expected [tunnel-%d] but found [tunnel-%d]",
                         i, profile->parsed_tunnel_indices[i]);
                return result;
            }
        }
    }
    
    /* Validate each tunnel */
    for (i = 0; i < profile->tunnel_count; i++) {
        tunnel_config_t *tunnel = &profile->tunnels[i];
        
        /* Check required parameters */
        if (!tunnel->nic_name || strlen(tunnel->nic_name) == 0) {
            result.error_code = BONDING_ERROR_MISSING_NIC;
            snprintf(result.error_message, sizeof(result.error_message),
                     "Missing NIC name for tunnel %d", i);
            result.line_number = i + 1;
            return result;
        }
        
        if (!tunnel->server_host || strlen(tunnel->server_host) == 0) {
            result.error_code = BONDING_ERROR_MISSING_PARAMETER;
            snprintf(result.error_message, sizeof(result.error_message),
                     "Missing server-host for tunnel %d", i);
            result.line_number = i + 1;
            return result;
        }
        
        if (tunnel->server_port < 1 || tunnel->server_port > 65535) {
            result.error_code = BONDING_ERROR_INVALID_PORT;
            snprintf(result.error_message, sizeof(result.error_message),
                     "Invalid server-port for tunnel %d: %d", i, tunnel->server_port);
            result.line_number = i + 1;
            return result;
        }
        
        if (!tunnel->config_file || strlen(tunnel->config_file) == 0) {
            result.error_code = BONDING_ERROR_MISSING_PARAMETER;
            snprintf(result.error_message, sizeof(result.error_message),
                     "Missing server-config for tunnel %d", i);
            result.line_number = i + 1;
            return result;
        }
        
        /* Check config file exists */
        wpath = Widen(tunnel->config_file);
        if (wpath) {
            if (!CheckFileAccess(wpath, GENERIC_READ)) {
                result.error_code = BONDING_ERROR_FILE_ACCESS;
                snprintf(result.error_message, sizeof(result.error_message),
                         "Config file not accessible for tunnel %d: %s", i, tunnel->config_file);
                result.line_number = i + 1;
                free(wpath);
                return result;
            }
            free(wpath);
        }
        
        /* Check weight */
        if (tunnel->weight <= 0) {
            result.error_code = BONDING_ERROR_INVALID_WEIGHT;
            snprintf(result.error_message, sizeof(result.error_message),
                     "Invalid weight for tunnel %d: %d", i, tunnel->weight);
            result.line_number = i + 1;
            return result;
        }
        
        /* Check for duplicate ports */
        for (j = i + 1; j < profile->tunnel_count; j++) {
            if (profile->tunnels[j].server_port == tunnel->server_port) {
                result.error_code = BONDING_ERROR_DUPLICATE_PORT;
                snprintf(result.error_message, sizeof(result.error_message),
                         "Duplicate server-port %d in tunnels %d and %d",
                         tunnel->server_port, i, j);
                result.line_number = j + 1;
                return result;
            }
        }
    }
    
    result.error_code = BONDING_ERROR_SUCCESS;
    strcpy(result.error_message, "Validation successful");
    return result;
}

/**
 * @brief Load configuration from file
 */
bonding_profile_t* bonding_config_load(const char *config_path)
{
    FILE *fd = NULL;
    char line[MAX_LINE_LENGTH];
    bonding_profile_t *profile = NULL;
    int first_line = 1;
    int in_tunnel_section = 0;
    int current_tunnel = -1;
    WCHAR *wpath;
    char *utf8_line;
    int line_num = 0;
    validation_result_t validation;
    /* Track parsed tunnel indices */
    int parsed_tunnel_indices[MAX_TUNNEL_COUNT];
    int parsed_tunnel_count = 0;
    int i;
    
    if (!config_path) {
        MsgToEventLog(EVENTLOG_ERROR_TYPE, L"bonding_config_load: config_path is NULL");
        return NULL;
    }
    
    /* Convert path to wide string */
    wpath = Widen(config_path);
    if (!wpath) {
        MsgToEventLog(EVENTLOG_ERROR_TYPE, L"bonding_config_load: Failed to convert path to wide string");
        return NULL;
    }
    
    /* Open file */
    if (_wfopen_s(&fd, wpath, L"r") != 0 || !fd) {
        MsgToEventLog(EVENTLOG_ERROR_TYPE, L"bonding_config_load: Failed to open file: %ls", wpath);
        free(wpath);
        return NULL;
    }
    
    free(wpath);
    
    /* Allocate profile structure */
    profile = (bonding_profile_t*)calloc(1, sizeof(bonding_profile_t));
    if (!profile) {
        MsgToEventLog(EVENTLOG_ERROR_TYPE, L"bonding_config_load: Out of memory");
        fclose(fd);
        return NULL;
    }
    
    /* Initialize defaults */
    profile->mode = BONDING_MODE_ROUND_ROBIN;
    profile->tunnel_count = 0;
    profile->config_path = duplicate_string(config_path);
    profile->packet_queue_size = 1000;  /* Default queue size */
    profile->packet_timeout = 100;      /* Default timeout in milliseconds */
    profile->sequencing_enabled = 1;    /* Default: enabled */
    profile->flow_control_enabled = 1;  /* Default: enabled */
    if (!profile->config_path) {
        free(profile);
        fclose(fd);
        return NULL;
    }
    
    /* Initialize tunnel tracking */
    for (i = 0; i < MAX_TUNNEL_COUNT; i++) {
        parsed_tunnel_indices[i] = -1;
    }
    parsed_tunnel_count = 0;
    
    /* Track parse failures */
    int parse_error_occurred = 0;
    int parse_error_line = 0;
    char parse_error_key[256] = {0};
    
    /* Read file line by line */
    while (fgets(line, sizeof(line), fd) != NULL) {
        line_num++;
        char *trimmed_line;
        char *key, *value;
        
        /* Remove UTF-8 BOM on first line */
        if (first_line && strncmp(line, "\xEF\xBB\xBF", 3) == 0) {
            memmove(line, line + 3, strlen(line) - 2);
        }
        first_line = 0;
        
        /* Trim line */
        trimmed_line = trim_whitespace(line);
        
        /* Skip comments and empty lines */
        if (is_comment_line(trimmed_line) || is_empty_line(trimmed_line))
            continue;
        
        /* Check for section header */
        if (trimmed_line[0] == '[') {
            int tunnel_idx;
            if (parse_section_header(trimmed_line, &tunnel_idx) == 0) {
                in_tunnel_section = 1;
                current_tunnel = tunnel_idx;
                
                /* Track parsed tunnel index */
                if (parsed_tunnel_count < MAX_TUNNEL_COUNT) {
                    parsed_tunnel_indices[parsed_tunnel_count] = tunnel_idx;
                    parsed_tunnel_count++;
                }
                
                /* Ensure tunnel array is allocated */
                if (!profile->tunnels && profile->tunnel_count > 0) {
                    profile->tunnels = (tunnel_config_t*)calloc(profile->tunnel_count, sizeof(tunnel_config_t));
                    if (!profile->tunnels) {
                        MsgToEventLog(EVENTLOG_ERROR_TYPE, L"bonding_config_load: Out of memory allocating tunnels");
                        bonding_config_free(profile);
                        fclose(fd);
                        return NULL;
                    }
                }
                
                if (current_tunnel >= 0 && current_tunnel < profile->tunnel_count) {
                    /* Initialize tunnel defaults */
                    profile->tunnels[current_tunnel].weight = 1;
                    profile->tunnels[current_tunnel].server_port = 0;
                    profile->tunnels[current_tunnel].state = TUNNEL_STATE_DISCONNECTED;
                }
            } else {
                /* Invalid section header */
                parse_error_occurred = 1;
                parse_error_line = line_num;
                MsgToEventLog(EVENTLOG_ERROR_TYPE, L"bonding_config_load: Invalid section header at line %d: %hs", line_num, trimmed_line);
            }
            continue;
        }
        
        /* Parse key-value pair */
        if (parse_line(trimmed_line, &key, &value) == 0) {
            if (in_tunnel_section && current_tunnel >= 0 && current_tunnel < profile->tunnel_count) {
                /* Parse tunnel parameter */
                if (parse_tunnel_parameter(&profile->tunnels[current_tunnel], key, value) != 0) {
                    parse_error_occurred = 1;
                    parse_error_line = line_num;
                    if (key) {
                        strncpy(parse_error_key, key, sizeof(parse_error_key) - 1);
                    }
                    MsgToEventLog(EVENTLOG_ERROR_TYPE, L"bonding_config_load: Parse error at line %d: %hs", line_num, trimmed_line);
                }
            } else {
                /* Parse global parameter */
                if (parse_global_parameter(profile, key, value) != 0) {
                    parse_error_occurred = 1;
                    parse_error_line = line_num;
                    if (key) {
                        strncpy(parse_error_key, key, sizeof(parse_error_key) - 1);
                    }
                    MsgToEventLog(EVENTLOG_ERROR_TYPE, L"bonding_config_load: Parse error at line %d: %hs", line_num, trimmed_line);
                }
            }
        } else {
            /* Failed to parse line (not a comment, not empty, not a section header) */
            parse_error_occurred = 1;
            parse_error_line = line_num;
            MsgToEventLog(EVENTLOG_ERROR_TYPE, L"bonding_config_load: Failed to parse line %d: %hs", line_num, trimmed_line);
        }
    }
    
    fclose(fd);
    
    /* Check for parse errors first */
    if (parse_error_occurred) {
        profile->validation.error_code = BONDING_ERROR_PARSE_ERROR;
        profile->validation.line_number = parse_error_line;
        if (parse_error_key[0] != '\0') {
            snprintf(profile->validation.error_message, sizeof(profile->validation.error_message),
                     "Parse error at line %d: invalid parameter '%s'", parse_error_line, parse_error_key);
        } else {
            snprintf(profile->validation.error_message, sizeof(profile->validation.error_message),
                     "Parse error at line %d: malformed line", parse_error_line);
        }
        wpath = Widen(profile->validation.error_message);
        if (wpath) {
            MsgToEventLog(EVENTLOG_ERROR_TYPE, L"bonding_config_load: Parse error: %ls", wpath);
            free(wpath);
        }
        bonding_config_free(profile);
        return NULL;
    }
    
    /* Store parsed tunnel information for validation */
    profile->parsed_tunnel_count = parsed_tunnel_count;
    for (i = 0; i < parsed_tunnel_count && i < MAX_TUNNEL_COUNT; i++) {
        profile->parsed_tunnel_indices[i] = parsed_tunnel_indices[i];
    }
    
    /* Validate configuration */
    validation = bonding_config_validate(profile);
    profile->validation = validation;
    
    if (validation.error_code != BONDING_ERROR_SUCCESS) {
        wpath = Widen(validation.error_message);
        if (wpath) {
            MsgToEventLog(EVENTLOG_ERROR_TYPE, L"bonding_config_load: Validation failed: %ls (line %d)", wpath, validation.line_number);
            free(wpath);
        }
        bonding_config_free(profile);
        return NULL;
    }
    
    return profile;
}

/**
 * @brief Save configuration to file
 */
int bonding_config_save(bonding_profile_t *profile, const char *config_path)
{
    FILE *fd = NULL;
    WCHAR *wpath;
    int i;
    time_t now;
    struct tm *timeinfo;
    char timestamp[64];
    
    if (!profile || !config_path)
        return -1;
    
    /* Convert path to wide string */
    wpath = Widen(config_path);
    if (!wpath)
        return -1;
    
    /* Open file for writing */
    if (_wfopen_s(&fd, wpath, L"w") != 0 || !fd) {
        MsgToEventLog(EVENTLOG_ERROR_TYPE, L"bonding_config_save: Failed to open file for writing: %ls", wpath);
        free(wpath);
        return -1;
    }
    
    free(wpath);
    
    /* Write header */
    time(&now);
    timeinfo = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);
    
    fprintf(fd, "# OpenVPN Channel Bonding Configuration\n");
    if (profile->profile_name) {
        fprintf(fd, "# Profile: %s\n", profile->profile_name);
    }
    fprintf(fd, "# Generated: %s\n", timestamp);
    fprintf(fd, "\n");
    
    /* Write global parameters */
    fprintf(fd, "# Bonding Mode\n");
    fprintf(fd, "# Options: round-robin, weighted, active-backup, adaptive\n");
    fprintf(fd, "bonding-mode %s\n", bonding_mode_to_string(profile->mode));
    fprintf(fd, "\n");
    
    fprintf(fd, "# Number of tunnels\n");
    fprintf(fd, "tunnel-count %d\n", profile->tunnel_count);
    fprintf(fd, "\n");
    
    /* Write packet distributor configuration */
    fprintf(fd, "# Packet Distributor Configuration\n");
    fprintf(fd, "# Maximum queue size (100-10000, default: 1000)\n");
    fprintf(fd, "packet-queue-size %d\n", profile->packet_queue_size);
    fprintf(fd, "# I/O timeout in milliseconds (default: 100)\n");
    fprintf(fd, "packet-timeout %d\n", profile->packet_timeout);
    fprintf(fd, "# Enable packet sequencing for server-side reordering (yes/no, default: yes)\n");
    fprintf(fd, "sequencing-enabled %s\n", profile->sequencing_enabled ? "yes" : "no");
    fprintf(fd, "# Enable flow control and backpressure (yes/no, default: yes)\n");
    fprintf(fd, "flow-control-enabled %s\n", profile->flow_control_enabled ? "yes" : "no");
    fprintf(fd, "\n");
    
    /* Write tunnel sections */
    for (i = 0; i < profile->tunnel_count; i++) {
        tunnel_config_t *tunnel = &profile->tunnels[i];
        
        fprintf(fd, "# Tunnel %d Configuration\n", i);
        fprintf(fd, "[tunnel-%d]\n", i);
        
        if (tunnel->nic_name) {
            fprintf(fd, "nic-name \"%s\"\n", tunnel->nic_name);
        }
        
        if (tunnel->server_host) {
            fprintf(fd, "server-host %s\n", tunnel->server_host);
        }
        
        fprintf(fd, "server-port %d\n", tunnel->server_port);
        
        if (tunnel->config_file) {
            fprintf(fd, "server-config %s\n", tunnel->config_file);
        }
        
        fprintf(fd, "weight %d\n", tunnel->weight);
        fprintf(fd, "\n");
    }
    
    if (fclose(fd) != 0) {
        MsgToEventLog(EVENTLOG_ERROR_TYPE, L"bonding_config_save: Failed to close file");
        return -1;
    }
    
    return 0;
}

/**
 * @brief Free configuration structure
 */
void bonding_config_free(bonding_profile_t *profile)
{
    int i;
    
    if (!profile)
        return;
    
    /* Free tunnel configurations */
    if (profile->tunnels) {
        for (i = 0; i < profile->tunnel_count; i++) {
            tunnel_config_t *tunnel = &profile->tunnels[i];
            
            if (tunnel->nic_name)
                free(tunnel->nic_name);
            if (tunnel->tap_adapter)
                free(tunnel->tap_adapter);
            if (tunnel->server_host)
                free(tunnel->server_host);
            if (tunnel->config_file)
                free(tunnel->config_file);
        }
        free(profile->tunnels);
    }
    
    /* Free profile strings */
    if (profile->profile_name)
        free(profile->profile_name);
    if (profile->config_path)
        free(profile->config_path);
    
    /* Free profile structure */
    free(profile);
}
