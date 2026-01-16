/**
 * @file test_config_parser.c
 * @brief Unit tests for configuration parser
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <windows.h>
#include "bonding_config.h"

/* Test result tracking */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, message) \
    do { \
        tests_run++; \
        if (condition) { \
            tests_passed++; \
            printf("PASS: %s\n", message); \
        } else { \
            tests_failed++; \
            printf("FAIL: %s\n", message); \
        } \
    } while(0)

/* Test helper: Create a temporary test file */
static char* create_test_file(const char *content)
{
    char *filename = (char*)malloc(MAX_PATH);
    char temp_path[MAX_PATH];
    FILE *f;
    
    if (!filename)
        return NULL;
    
    GetTempPathA(MAX_PATH, temp_path);
    sprintf(filename, "%stest_bonding_%d.ovpn-bond", temp_path, GetCurrentProcessId());
    
    f = fopen(filename, "w");
    if (!f) {
        free(filename);
        return NULL;
    }
    
    fprintf(f, "%s", content);
    fclose(f);
    
    return filename;
}

/* Test helper: Create a temporary base.ovpn file for tests that reference server-config */
static char* create_base_ovpn_file(const char *config_dir)
{
    char *filename = (char*)malloc(MAX_PATH);
    FILE *f;
    
    if (!filename || !config_dir)
        return NULL;
    
    sprintf(filename, "%sbase.ovpn", config_dir);
    
    f = fopen(filename, "w");
    if (!f) {
        free(filename);
        return NULL;
    }
    
    /* Write minimal valid OpenVPN config */
    fprintf(f, "# Base OpenVPN configuration for testing\n");
    fprintf(f, "remote vpn.example.com\n");
    fprintf(f, "proto udp\n");
    fprintf(f, "dev tun\n");
    fclose(f);
    
    return filename;
}

/* Test helper: Create a temporary config file with specific name */
static char* create_config_file(const char *config_dir, const char *config_name)
{
    char *filename = (char*)malloc(MAX_PATH);
    FILE *f;
    
    if (!filename || !config_dir || !config_name)
        return NULL;
    
    sprintf(filename, "%s%s", config_dir, config_name);
    
    f = fopen(filename, "w");
    if (!f) {
        free(filename);
        return NULL;
    }
    
    /* Write minimal valid OpenVPN config */
    fprintf(f, "# Base OpenVPN configuration for testing\n");
    fprintf(f, "remote vpn.example.com\n");
    fprintf(f, "proto udp\n");
    fprintf(f, "dev tun\n");
    fclose(f);
    
    return filename;
}

/* Test helper: Delete test file */
static void delete_test_file(const char *filename)
{
    if (filename)
        DeleteFileA(filename);
    free((void*)filename);
}

/* Test 1: Load valid configuration */
static void test_load_valid_config(void)
{
    char temp_path[MAX_PATH];
    char *base_ovpn_path = NULL;
    const char *config_content =
        "# Test Configuration\n"
        "bonding-mode round-robin\n"
        "tunnel-count 2\n"
        "\n"
        "[tunnel-0]\n"
        "nic-name \"Ethernet\"\n"
        "server-host vpn.example.com\n"
        "server-port 1194\n"
        "server-config base.ovpn\n"
        "weight 1\n"
        "\n"
        "[tunnel-1]\n"
        "nic-name \"Wi-Fi\"\n"
        "server-host vpn.example.com\n"
        "server-port 1195\n"
        "server-config base.ovpn\n"
        "weight 1\n";
    
    char *filename = create_test_file(config_content);
    bonding_profile_t *profile = NULL;
    
    if (!filename) {
        TEST_ASSERT(0, "test_load_valid_config: Failed to create test file");
        return;
    }
    
    /* Create base.ovpn file in same directory */
    GetTempPathA(MAX_PATH, temp_path);
    base_ovpn_path = create_base_ovpn_file(temp_path);
    
    profile = bonding_config_load(filename);
    TEST_ASSERT(profile != NULL, "test_load_valid_config: Profile loaded successfully");
    
    if (profile) {
        TEST_ASSERT(profile->mode == BONDING_MODE_ROUND_ROBIN, "test_load_valid_config: Mode is round-robin");
        TEST_ASSERT(profile->tunnel_count == 2, "test_load_valid_config: Tunnel count is 2");
        TEST_ASSERT(profile->tunnels != NULL, "test_load_valid_config: Tunnels array allocated");
        
        if (profile->tunnels) {
            TEST_ASSERT(strcmp(profile->tunnels[0].nic_name, "Ethernet") == 0, "test_load_valid_config: Tunnel 0 NIC name");
            TEST_ASSERT(profile->tunnels[0].server_port == 1194, "test_load_valid_config: Tunnel 0 port");
            TEST_ASSERT(strcmp(profile->tunnels[1].nic_name, "Wi-Fi") == 0, "test_load_valid_config: Tunnel 1 NIC name");
            TEST_ASSERT(profile->tunnels[1].server_port == 1195, "test_load_valid_config: Tunnel 1 port");
        }
        
        bonding_config_free(profile);
    }
    
    delete_test_file(filename);
    if (base_ovpn_path) {
        delete_test_file(base_ovpn_path);
    }
}

/* Test 2: Parse global parameters */
static void test_parse_global_parameters(void)
{
    const char *config_content =
        "bonding-mode weighted\n"
        "tunnel-count 3\n";
    
    char *filename = create_test_file(config_content);
    bonding_profile_t *profile = NULL;
    
    if (!filename) {
        TEST_ASSERT(0, "test_parse_global_parameters: Failed to create test file");
        return;
    }
    
    profile = bonding_config_load(filename);
    TEST_ASSERT(profile != NULL, "test_parse_global_parameters: Profile loaded");
    
    if (profile) {
        TEST_ASSERT(profile->mode == BONDING_MODE_WEIGHTED, "test_parse_global_parameters: Mode is weighted");
        TEST_ASSERT(profile->tunnel_count == 3, "test_parse_global_parameters: Tunnel count is 3");
        bonding_config_free(profile);
    }
    
    delete_test_file(filename);
}

/* Test 3: Parse tunnel sections */
static void test_parse_tunnel_sections(void)
{
    char temp_path[MAX_PATH];
    char *base_ovpn_path1 = NULL, *base_ovpn_path2 = NULL;
    const char *config_content =
        "bonding-mode round-robin\n"
        "tunnel-count 2\n"
        "\n"
        "[tunnel-0]\n"
        "nic-name \"Ethernet\"\n"
        "server-host server1.example.com\n"
        "server-port 1194\n"
        "server-config config1.ovpn\n"
        "weight 2\n"
        "\n"
        "[tunnel-1]\n"
        "nic-name \"Wi-Fi\"\n"
        "server-host server2.example.com\n"
        "server-port 1195\n"
        "server-config config2.ovpn\n"
        "weight 3\n";
    
    char *filename = create_test_file(config_content);
    bonding_profile_t *profile = NULL;
    
    if (!filename) {
        TEST_ASSERT(0, "test_parse_tunnel_sections: Failed to create test file");
        return;
    }
    
    /* Create config files in same directory */
    GetTempPathA(MAX_PATH, temp_path);
    base_ovpn_path1 = create_config_file(temp_path, "config1.ovpn");
    base_ovpn_path2 = create_config_file(temp_path, "config2.ovpn");
    
    profile = bonding_config_load(filename);
    TEST_ASSERT(profile != NULL, "test_parse_tunnel_sections: Profile loaded");
    
    if (profile && profile->tunnels) {
        TEST_ASSERT(strcmp(profile->tunnels[0].server_host, "server1.example.com") == 0, "test_parse_tunnel_sections: Tunnel 0 host");
        TEST_ASSERT(profile->tunnels[0].weight == 2, "test_parse_tunnel_sections: Tunnel 0 weight");
        TEST_ASSERT(strcmp(profile->tunnels[1].server_host, "server2.example.com") == 0, "test_parse_tunnel_sections: Tunnel 1 host");
        TEST_ASSERT(profile->tunnels[1].weight == 3, "test_parse_tunnel_sections: Tunnel 1 weight");
        bonding_config_free(profile);
    }
    
    delete_test_file(filename);
    if (base_ovpn_path1) delete_test_file(base_ovpn_path1);
    if (base_ovpn_path2) delete_test_file(base_ovpn_path2);
}

/* Test 4: Handle missing file */
static void test_missing_file(void)
{
    bonding_profile_t *profile = bonding_config_load("nonexistent_file.ovpn-bond");
    TEST_ASSERT(profile == NULL, "test_missing_file: Returns NULL for missing file");
}

/* Test 5: Handle malformed configuration */
static void test_malformed_config(void)
{
    const char *config_content = "invalid line without equals\n";
    char *filename = create_test_file(config_content);
    bonding_profile_t *profile = NULL;
    
    if (!filename) {
        TEST_ASSERT(0, "test_malformed_config: Failed to create test file");
        return;
    }
    
    profile = bonding_config_load(filename);
    /* Should either return NULL or load with validation errors */
    TEST_ASSERT(profile == NULL || profile->validation.error_code != BONDING_ERROR_SUCCESS,
                "test_malformed_config: Handles malformed config");
    
    if (profile)
        bonding_config_free(profile);
    
    delete_test_file(filename);
}

/* Test 6: Comment and empty line handling */
static void test_comments_and_empty_lines(void)
{
    char temp_path[MAX_PATH];
    char *base_ovpn_path = NULL;
    const char *config_content =
        "# This is a comment\n"
        "; This is also a comment\n"
        "\n"
        "bonding-mode round-robin\n"
        "\n"
        "tunnel-count 1\n"
        "\n"
        "[tunnel-0]\n"
        "# Tunnel comment\n"
        "nic-name \"Ethernet\"\n"
        "server-host vpn.example.com\n"
        "server-port 1194\n"
        "server-config base.ovpn\n"
        "weight 1\n";
    
    char *filename = create_test_file(config_content);
    bonding_profile_t *profile = NULL;
    
    if (!filename) {
        TEST_ASSERT(0, "test_comments_and_empty_lines: Failed to create test file");
        return;
    }
    
    /* Create base.ovpn file in same directory */
    GetTempPathA(MAX_PATH, temp_path);
    base_ovpn_path = create_base_ovpn_file(temp_path);
    
    profile = bonding_config_load(filename);
    TEST_ASSERT(profile != NULL, "test_comments_and_empty_lines: Profile loaded with comments");
    
    if (profile) {
        TEST_ASSERT(profile->mode == BONDING_MODE_ROUND_ROBIN, "test_comments_and_empty_lines: Mode parsed correctly");
        TEST_ASSERT(profile->tunnel_count == 1, "test_comments_and_empty_lines: Tunnel count parsed correctly");
        bonding_config_free(profile);
    }
    
    delete_test_file(filename);
    if (base_ovpn_path) delete_test_file(base_ovpn_path);
}

/* Test 7: UTF-8 BOM handling */
static void test_utf8_bom(void)
{
    FILE *f;
    char *filename = (char*)malloc(MAX_PATH);
    char temp_path[MAX_PATH];
    char *base_ovpn_path = NULL;
    bonding_profile_t *profile = NULL;
    
    if (!filename) {
        TEST_ASSERT(0, "test_utf8_bom: Failed to allocate filename");
        return;
    }
    
    GetTempPathA(MAX_PATH, temp_path);
    sprintf(filename, "%stest_bom_%d.ovpn-bond", temp_path, GetCurrentProcessId());
    
    f = fopen(filename, "wb");
    if (!f) {
        free(filename);
        TEST_ASSERT(0, "test_utf8_bom: Failed to create test file");
        return;
    }
    
    /* Write UTF-8 BOM */
    fprintf(f, "\xEF\xBB\xBF");
    fprintf(f, "bonding-mode round-robin\n");
    fprintf(f, "tunnel-count 1\n");
    fprintf(f, "[tunnel-0]\n");
    fprintf(f, "nic-name \"Ethernet\"\n");
    fprintf(f, "server-host vpn.example.com\n");
    fprintf(f, "server-port 1194\n");
    fprintf(f, "server-config base.ovpn\n");
    fprintf(f, "weight 1\n");
    fclose(f);
    
    /* Create base.ovpn file in same directory */
    base_ovpn_path = create_base_ovpn_file(temp_path);
    
    profile = bonding_config_load(filename);
    TEST_ASSERT(profile != NULL, "test_utf8_bom: Profile loaded with BOM");
    
    if (profile) {
        TEST_ASSERT(profile->mode == BONDING_MODE_ROUND_ROBIN, "test_utf8_bom: Mode parsed correctly");
        bonding_config_free(profile);
    }
    
    delete_test_file(filename);
    if (base_ovpn_path) delete_test_file(base_ovpn_path);
}

/* Test 8: Save and reload cycle */
static void test_save_and_reload(void)
{
    char temp_path[MAX_PATH];
    char *base_ovpn_path = NULL;
    const char *config_content =
        "bonding-mode active-backup\n"
        "tunnel-count 2\n"
        "\n"
        "[tunnel-0]\n"
        "nic-name \"Ethernet\"\n"
        "server-host vpn1.example.com\n"
        "server-port 1194\n"
        "server-config base.ovpn\n"
        "weight 1\n"
        "\n"
        "[tunnel-1]\n"
        "nic-name \"Wi-Fi\"\n"
        "server-host vpn2.example.com\n"
        "server-port 1195\n"
        "server-config base.ovpn\n"
        "weight 1\n";
    
    char *filename1 = create_test_file(config_content);
    char *filename2 = (char*)malloc(MAX_PATH);
    bonding_profile_t *profile1 = NULL;
    bonding_profile_t *profile2 = NULL;
    
    if (!filename1 || !filename2) {
        TEST_ASSERT(0, "test_save_and_reload: Failed to create test files");
        if (filename1) free(filename1);
        if (filename2) free(filename2);
        return;
    }
    
    GetTempPathA(MAX_PATH, temp_path);
    sprintf(filename2, "%stest_reload_%d.ovpn-bond", temp_path, GetCurrentProcessId());
    
    /* Create base.ovpn file in same directory */
    base_ovpn_path = create_base_ovpn_file(temp_path);
    
    /* Load original */
    profile1 = bonding_config_load(filename1);
    TEST_ASSERT(profile1 != NULL, "test_save_and_reload: Original profile loaded");
    
    if (profile1) {
        /* Save to new file */
        int save_result = bonding_config_save(profile1, filename2);
        TEST_ASSERT(save_result == 0, "test_save_and_reload: Profile saved successfully");
        
        if (save_result == 0) {
            /* Reload from saved file */
            profile2 = bonding_config_load(filename2);
            TEST_ASSERT(profile2 != NULL, "test_save_and_reload: Reloaded profile loaded");
            
            if (profile2) {
                TEST_ASSERT(profile2->mode == profile1->mode, "test_save_and_reload: Mode matches");
                TEST_ASSERT(profile2->tunnel_count == profile1->tunnel_count, "test_save_and_reload: Tunnel count matches");
                
                if (profile2->tunnels && profile1->tunnels) {
                    TEST_ASSERT(strcmp(profile2->tunnels[0].nic_name, profile1->tunnels[0].nic_name) == 0,
                                "test_save_and_reload: Tunnel 0 NIC name matches");
                    TEST_ASSERT(profile2->tunnels[0].server_port == profile1->tunnels[0].server_port,
                                "test_save_and_reload: Tunnel 0 port matches");
                }
                
                bonding_config_free(profile2);
            }
        }
        
        bonding_config_free(profile1);
    }
    
    delete_test_file(filename1);
    delete_test_file(filename2);
    if (base_ovpn_path) delete_test_file(base_ovpn_path);
}

/* Test 9: Memory cleanup */
static void test_memory_cleanup(void)
{
    char temp_path[MAX_PATH];
    char *base_ovpn_path = NULL;
    const char *config_content =
        "bonding-mode round-robin\n"
        "tunnel-count 2\n"
        "\n"
        "[tunnel-0]\n"
        "nic-name \"Ethernet\"\n"
        "server-host vpn.example.com\n"
        "server-port 1194\n"
        "server-config base.ovpn\n"
        "weight 1\n"
        "\n"
        "[tunnel-1]\n"
        "nic-name \"Wi-Fi\"\n"
        "server-host vpn.example.com\n"
        "server-port 1195\n"
        "server-config base.ovpn\n"
        "weight 1\n";
    
    char *filename = create_test_file(config_content);
    bonding_profile_t *profile = NULL;
    
    if (!filename) {
        TEST_ASSERT(0, "test_memory_cleanup: Failed to create test file");
        return;
    }
    
    /* Create base.ovpn file in same directory */
    GetTempPathA(MAX_PATH, temp_path);
    base_ovpn_path = create_base_ovpn_file(temp_path);
    
    profile = bonding_config_load(filename);
    TEST_ASSERT(profile != NULL, "test_memory_cleanup: Profile loaded");
    
    /* Free should not crash */
    bonding_config_free(profile);
    bonding_config_free(NULL); /* Should handle NULL gracefully */
    
    TEST_ASSERT(1, "test_memory_cleanup: Memory cleanup completed without crash");
    
    delete_test_file(filename);
    if (base_ovpn_path) delete_test_file(base_ovpn_path);
}

/* Test 10: Mode conversion functions */
static void test_mode_conversion(void)
{
    bonding_mode_t mode;
    const char *str;
    
    mode = bonding_mode_from_string("round-robin");
    TEST_ASSERT(mode == BONDING_MODE_ROUND_ROBIN, "test_mode_conversion: round-robin string to enum");
    
    mode = bonding_mode_from_string("weighted");
    TEST_ASSERT(mode == BONDING_MODE_WEIGHTED, "test_mode_conversion: weighted string to enum");
    
    mode = bonding_mode_from_string("active-backup");
    TEST_ASSERT(mode == BONDING_MODE_ACTIVE_BACKUP, "test_mode_conversion: active-backup string to enum");
    
    str = bonding_mode_to_string(BONDING_MODE_ROUND_ROBIN);
    TEST_ASSERT(strcmp(str, "round-robin") == 0, "test_mode_conversion: round-robin enum to string");
    
    str = bonding_mode_to_string(BONDING_MODE_WEIGHTED);
    TEST_ASSERT(strcmp(str, "weighted") == 0, "test_mode_conversion: weighted enum to string");
    
    str = bonding_mode_to_string(BONDING_MODE_ACTIVE_BACKUP);
    TEST_ASSERT(strcmp(str, "active-backup") == 0, "test_mode_conversion: active-backup enum to string");
}

/* Main test runner */
int main(void)
{
    printf("=== Bonding Configuration Parser Tests ===\n\n");
    
    test_load_valid_config();
    test_parse_global_parameters();
    test_parse_tunnel_sections();
    test_missing_file();
    test_malformed_config();
    test_comments_and_empty_lines();
    test_utf8_bom();
    test_save_and_reload();
    test_memory_cleanup();
    test_mode_conversion();
    
    printf("\n=== Test Results ===\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);
    
    if (tests_failed == 0) {
        printf("\nAll tests passed!\n");
        return 0;
    } else {
        printf("\nSome tests failed!\n");
        return 1;
    }
}
