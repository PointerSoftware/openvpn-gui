#ifndef ROUTING_HELPER_H
#define ROUTING_HELPER_H

/**
 * @file routing_helper.h
 * @brief Windows routing API helper functions
 */

/* Routing functions */
int routing_helper_bind_to_nic(const char *nic_name, const char *destination);
int routing_helper_remove_binding(const char *nic_name, const char *destination);
int routing_helper_restore_routes(void);
int routing_helper_set_metric(const char *nic_name, const char *destination, int metric);
int routing_helper_update_nic_metrics(const char *nic_name, int priority);

#endif /* ROUTING_HELPER_H */
