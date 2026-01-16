# Bonding Configuration

## Configuration File Format

Bonding profiles use the `.ovpn-bond` file extension and extend the standard OpenVPN configuration format with bonding-specific parameters.

## File Structure

```
# OpenVPN Channel Bonding Configuration
# Profile: <profile_name>

# Bonding Mode
# Options: round-robin, weighted, active-backup, adaptive
bonding-mode round-robin

# Number of tunnels
tunnel-count 3

# Tunnel 0 Configuration
[tunnel-0]
nic-name "Ethernet"
server-host vpn.example.com
server-port 1194
server-config server1.ovpn
weight 1

# Tunnel 1 Configuration
[tunnel-1]
nic-name "Wi-Fi"
server-host vpn.example.com
server-port 1195
server-config server2.ovpn
weight 1

# Tunnel 2 Configuration
[tunnel-2]
nic-name "Ethernet 2"
server-host vpn.example.com
server-port 1196
server-config server3.ovpn
weight 2

# Standard OpenVPN parameters (applied to all tunnels)
remote vpn.example.com
proto udp
dev tun
ca ca.crt
cert client.crt
key client.key
```

## Configuration Parameters

### Global Parameters

- **bonding-mode**: Bonding distribution algorithm
  - `round-robin`: Sequential packet distribution
  - `weighted`: Weight-based distribution
  - `active-backup`: Active/standby redundancy
  - `adaptive`: Adaptive bonding mode

- **tunnel-count**: Number of tunnels to create (must match number of `[tunnel-N]` sections)

### Per-Tunnel Parameters

Each tunnel section `[tunnel-N]` contains:

- **nic-name**: Physical NIC name to bind this tunnel to
  - Must match a physical NIC name detected by the system
  - Examples: "Ethernet", "Wi-Fi", "Local Area Connection"

- **server-host**: OpenVPN server hostname or IP address
  - Can be same for all tunnels (different ports) or different servers

- **server-port**: OpenVPN server port for this tunnel
  - Must be unique per tunnel
  - Example: 1194, 1195, 1196

- **server-config**: Path to base OpenVPN configuration file
  - Contains common OpenVPN parameters
  - Tunnel-specific parameters override base config

- **weight**: Distribution weight (for weighted mode)
  - Higher weight = more traffic
  - Default: 1
  - Ignored in round-robin mode

## Example Configuration Files

### Simple Round-Robin (2 Tunnels)

```
# Simple 2-tunnel bonding
bonding-mode round-robin
tunnel-count 2

[tunnel-0]
nic-name "Ethernet"
server-host vpn.example.com
server-port 1194
server-config base.ovpn
weight 1

[tunnel-1]
nic-name "Wi-Fi"
server-host vpn.example.com
server-port 1195
server-config base.ovpn
weight 1
```

### Weighted Distribution (3 Tunnels)

```
# Weighted bonding with 3 tunnels
bonding-mode weighted
tunnel-count 3

[tunnel-0]
nic-name "Ethernet"
server-host vpn.example.com
server-port 1194
server-config base.ovpn
weight 3

[tunnel-1]
nic-name "Wi-Fi"
server-host vpn.example.com
server-port 1195
server-config base.ovpn
weight 2

[tunnel-2]
nic-name "LTE"
server-host vpn.example.com
server-port 1196
server-config base.ovpn
weight 1
```

### Active-Backup Redundancy

```
# Active-backup for redundancy
bonding-mode active-backup
tunnel-count 2

[tunnel-0]
nic-name "Ethernet"
server-host vpn.example.com
server-port 1194
server-config base.ovpn
weight 1

[tunnel-1]
nic-name "Wi-Fi"
server-host vpn.example.com
server-port 1195
server-config base.ovpn
weight 1
```

## Configuration Validation

The configuration parser validates:

1. **Required parameters**: All global and per-tunnel parameters must be present
2. **NIC names**: Must match detected physical NICs
3. **Port uniqueness**: Each tunnel must have a unique server port
4. **Tunnel count**: Must match number of tunnel sections
5. **File paths**: Server-config files must exist and be readable
6. **Mode compatibility**: Weight parameter required for weighted mode

### Validation Rules in Detail

#### Global Parameters
- `bonding-mode`: Must be one of: `round-robin`, `weighted`, `active-backup`, `adaptive`
- `tunnel-count`: Must be between 1 and 32 (MAX_TUNNEL_COUNT)

#### Per-Tunnel Parameters
- `nic-name`: Required, must be non-empty string
- `server-host`: Required, must be non-empty string (hostname or IP address)
- `server-port`: Required, must be between 1 and 65535
- `server-config`: Required, must be non-empty string, file must exist and be readable
- `weight`: Required, must be positive integer (> 0)

#### Cross-Tunnel Validation
- All server ports must be unique across all tunnels
- Number of `[tunnel-N]` sections must match `tunnel-count` value
- Tunnel indices must be sequential starting from 0 (e.g., [tunnel-0], [tunnel-1], ...)

## Error Handling

### Error Codes

The configuration parser uses the following error codes (defined in `bonding_error_t`):

- `BONDING_ERROR_SUCCESS` (0): Operation completed successfully
- `BONDING_ERROR_FILE_NOT_FOUND`: Configuration file does not exist
- `BONDING_ERROR_PARSE_ERROR`: Syntax error in configuration file
- `BONDING_ERROR_VALIDATION_ERROR`: Configuration failed validation
- `BONDING_ERROR_MEMORY_ERROR`: Out of memory
- `BONDING_ERROR_INVALID_MODE`: Invalid bonding mode specified
- `BONDING_ERROR_INVALID_TUNNEL_COUNT`: Tunnel count out of valid range
- `BONDING_ERROR_DUPLICATE_PORT`: Duplicate server port detected
- `BONDING_ERROR_MISSING_NIC`: Missing or empty NIC name
- `BONDING_ERROR_MISSING_PARAMETER`: Required parameter missing
- `BONDING_ERROR_INVALID_PORT`: Server port out of valid range (1-65535)
- `BONDING_ERROR_INVALID_WEIGHT`: Weight value invalid (must be > 0)
- `BONDING_ERROR_FILE_ACCESS`: Config file not accessible or readable
- `BONDING_ERROR_WRITE_ERROR`: Failed to write configuration file

### Error Messages

When validation fails, the `validation_result_t` structure contains:
- `error_code`: Specific error code
- `line_number`: Line number where error occurred (if applicable)
- `error_message`: Human-readable error description

### Common Configuration Errors

#### Missing Required Parameter
```
Error: Missing server-host for tunnel 1
```
**Solution**: Ensure all required parameters are present for each tunnel section.

#### Duplicate Port
```
Error: Duplicate server-port 1194 in tunnels 0 and 1
```
**Solution**: Use unique server ports for each tunnel.

#### Invalid Tunnel Count
```
Error: Invalid tunnel count: 5 (must be 1-32)
```
**Solution**: Reduce tunnel count to 32 or fewer.

#### File Not Found
```
Error: Config file not accessible for tunnel 0: base.ovpn
```
**Solution**: Verify the `server-config` file path is correct and the file exists.

#### Invalid Bonding Mode
```
Error: Invalid bonding mode
```
**Solution**: Use one of: `round-robin`, `weighted`, `active-backup`, `adaptive`

## API Usage Examples

### Loading a Configuration

```c
#include "bonding_config.h"

/* Load configuration from file */
bonding_profile_t *profile = bonding_config_load("configs/my-bond.ovpn-bond");

if (profile == NULL) {
    /* Handle error - check validation result */
    printf("Failed to load configuration\n");
    return -1;
}

/* Use profile */
printf("Profile mode: %s\n", bonding_mode_to_string(profile->mode));
printf("Tunnel count: %d\n", profile->tunnel_count);

/* Access tunnel configurations */
for (int i = 0; i < profile->tunnel_count; i++) {
    tunnel_config_t *tunnel = &profile->tunnels[i];
    printf("Tunnel %d: %s:%d\n", i, tunnel->server_host, tunnel->server_port);
}

/* Free when done */
bonding_config_free(profile);
```

### Saving a Configuration

```c
/* Create or modify profile */
bonding_profile_t *profile = calloc(1, sizeof(bonding_profile_t));
profile->mode = BONDING_MODE_WEIGHTED;
profile->tunnel_count = 2;
/* ... populate tunnels ... */

/* Save to file */
int result = bonding_config_save(profile, "configs/saved-bond.ovpn-bond");
if (result != 0) {
    printf("Failed to save configuration\n");
}

bonding_config_free(profile);
```

### Validating a Configuration

```c
bonding_profile_t *profile = bonding_config_load("configs/my-bond.ovpn-bond");
if (profile == NULL) {
    return -1;
}

/* Validate configuration */
validation_result_t validation = bonding_config_validate(profile);

if (validation.error_code != BONDING_ERROR_SUCCESS) {
    printf("Validation failed at line %d: %s\n", 
           validation.line_number, 
           validation.error_message);
    bonding_config_free(profile);
    return -1;
}

/* Configuration is valid */
```

### Error Handling Pattern

```c
bonding_profile_t *profile = bonding_config_load(path);
if (profile == NULL) {
    /* Check validation result if available */
    /* Error logged to event log via MsgToEventLog() */
    return NULL;
}

/* Check validation state */
if (profile->validation.error_code != BONDING_ERROR_SUCCESS) {
    /* Handle validation error */
    printf("Warning: Configuration has validation issues\n");
}

/* Use profile */
/* ... */

/* Always free when done */
bonding_config_free(profile);
```

## Memory Management

### Allocation

- `bonding_config_load()` allocates all necessary memory for the profile structure
- All strings (nic_name, server_host, config_file, etc.) are dynamically allocated
- Tunnel array is allocated based on `tunnel-count` parameter

### Deallocation

- Always call `bonding_config_free()` when done with a profile
- `bonding_config_free()` handles NULL pointers gracefully
- All allocated memory is properly freed, including nested structures

### Best Practices

1. **Always free profiles**: Call `bonding_config_free()` after using a profile
2. **Check for NULL**: Always check if `bonding_config_load()` returns NULL
3. **Validate after loading**: Check `profile->validation.error_code` after loading
4. **Don't modify strings directly**: Strings are owned by the profile structure
5. **Thread safety**: Configuration functions are not thread-safe (use synchronization if needed)

## Troubleshooting

### Configuration File Not Loading

1. **Check file path**: Verify the path is correct and file exists
2. **Check file permissions**: Ensure read access to the configuration file
3. **Check file encoding**: File should be UTF-8 (BOM is handled automatically)
4. **Check syntax**: Verify INI-style syntax is correct
5. **Check event log**: Errors are logged via `MsgToEventLog()` - check Windows Event Viewer

### Validation Failures

1. **Read error message**: Check `validation_result_t.error_message` for specific issue
2. **Check line numbers**: Error messages include line numbers when applicable
3. **Verify required parameters**: Ensure all required parameters are present
4. **Check file paths**: Verify `server-config` files exist and are accessible
5. **Check port uniqueness**: Ensure all tunnels have unique server ports

### Common Issues

#### Issue: "Missing NIC name for tunnel X"
**Cause**: `nic-name` parameter missing or empty in tunnel section
**Solution**: Add `nic-name "YourNICName"` to the tunnel section

#### Issue: "Duplicate server-port"
**Cause**: Multiple tunnels using the same port number
**Solution**: Use unique ports for each tunnel (e.g., 1194, 1195, 1196)

#### Issue: "Config file not accessible"
**Cause**: `server-config` file path is incorrect or file doesn't exist
**Solution**: Verify the path is correct relative to the `.ovpn-bond` file location

#### Issue: "Invalid tunnel count"
**Cause**: `tunnel-count` doesn't match number of `[tunnel-N]` sections
**Solution**: Ensure `tunnel-count` matches the number of tunnel sections defined

## Configuration Generation

When a user configures bonding through the GUI:

1. User selects physical NICs
2. User configures server endpoints
3. GUI generates `.ovpn-bond` file
4. File is saved to OpenVPN config directory
5. Configuration is sent to bonding service

## Integration with OpenVPN Configs

Each tunnel uses a base OpenVPN configuration file (specified in `server-config`). The bonding system:

1. Loads base OpenVPN config
2. Overrides tunnel-specific parameters:
   - `--dev-node <tap-adapter>`: Binds to specific TAP adapter
   - `--remote <server-host> <server-port>`: Sets server endpoint
   - `--nobind`: Allows binding to specific interface
3. Generates temporary config for OpenVPN process
4. Spawns OpenVPN with generated config

## Server-Side Requirements

For each tunnel, the server must:

1. Run OpenVPN server instance on specified port
2. Use separate TUN interface (tun0, tun1, tun2, etc.)
3. Configure Linux bonding to aggregate tunnel interfaces
4. Use same CA for certificate validation

See `architecture.md` for server-side configuration details.
