/* AUTO-GENERATED; DO NOT EDIT. */
#ifndef AUTOLINK_SEMANTICS_GENERATED_H
#define AUTOLINK_SEMANTICS_GENERATED_H
#include <stddef.h>
#include <stdint.h>
#include <gs_proto.h>
typedef struct { uint8_t func, op, txn, length_kind, address_role; uint16_t length; } al_request_contract_t;
typedef struct { uint8_t normal_func, allow_exception; } al_response_contract_t;
enum { AL_TXN_UNICAST=1, AL_TXN_UID_DIRECTED=2, AL_TXN_COLLECT=3, AL_TXN_BROADCAST_NOACK=4 };
enum { AL_LEN_FIXED=1, AL_LEN_TYPED=2, AL_LEN_RANGE=3 };
enum { AL_ADDR_CURRENT=1, AL_ADDR_UNASSIGNED=2, AL_ADDR_BROADCAST=3, AL_ADDR_UID_BROADCAST=4 };
#define AL_SLOT_ACCURACY_US 500u
#define AL_TWO_NODE_RELATIVE_BOUND_US 1000u
#define AL_MASTER_WORK_POLL_INTERVAL_MS 1000u
#define AL_MASTER_SLEEP_VERIFY_BASE_MS 1000u
#define AL_MASTER_SLEEP_VERIFY_MAX_MS 60000u
extern const al_request_contract_t al_request_contracts[];
extern const size_t al_request_contract_count;
extern const al_response_contract_t al_response_contracts[];
const al_request_contract_t *al_find_request_contract(uint8_t func, uint8_t op);
int al_response_func_allowed(uint8_t request_func, uint8_t op, uint8_t response_func);
int al_response_payload_allowed(uint8_t request_func, uint8_t op,
                                const uint8_t *request_data, uint16_t request_len,
                                uint8_t response_addr,
                                const uint8_t *response_data, uint16_t response_len);
int al_is_known_request(uint8_t func);
int al_request_length_allowed(uint8_t func, const uint8_t *data,
                              uint16_t length);
int al_node_state_allowed(uint8_t func, uint8_t commission, uint8_t service);
int al_is_generic_idempotent(uint8_t func);
int al_candidate_confirmation_eligible(uint8_t func, uint8_t op);
int al_is_wire_error(uint8_t code);
int al_error_allowed(uint8_t request_func, uint8_t code);
uint8_t al_config_request_length(uint8_t config_id);
uint8_t al_data_type_width(uint8_t data_type);
int al_is_supported_baud(uint32_t baud);
size_t al_supported_baud_count(void);
uint32_t al_supported_baud_at(size_t index);
#endif
