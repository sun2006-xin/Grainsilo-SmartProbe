/* AUTO-GENERATED; DO NOT EDIT. */
#include "autolink_semantics.h"
#include <string.h>
typedef struct { uint8_t func, code; } al_error_pair_t;
typedef struct { uint8_t func, commission_mask, service_mask; } al_state_policy_t;
const al_request_contract_t al_request_contracts[] = {
  {3, 0, 1, 1, 1, 0},
  {4, 0, 1, 1, 1, 1},
  {6, 0, 1, 2, 1, 0},
  {8, 0, 1, 3, 1, 0},
  {23, 0, 1, 1, 1, 4},
  {26, 0, 1, 1, 1, 0},
  {17, 0, 2, 1, 2, 15},
  {19, 0, 1, 1, 1, 0},
  {28, 1, 4, 1, 3, 14},
  {28, 3, 1, 1, 1, 10},
  {28, 2, 4, 1, 3, 16},
  {5, 0, 3, 1, 3, 13},
  {7, 0, 3, 1, 3, 13},
  {30, 0, 3, 1, 3, 14},
  {29, 0, 2, 1, 4, 15}
};
const size_t al_request_contract_count =
    sizeof(al_request_contracts) / sizeof(al_request_contracts[0]);
const al_response_contract_t al_response_contracts[] = {
  {3, 1},
  {4, 1},
  {6, 1},
  {8, 1},
  {23, 1},
  {0, 1},
  {18, 1},
  {19, 1},
  {0, 0},
  {28, 1},
  {0, 0},
  {16, 0},
  {16, 0},
  {16, 0},
  {18, 1}
};
static const al_error_pair_t al_error_pairs[] = {
  {3, 2},
  {4, 2},
  {6, 2},
  {8, 2},
  {17, 2},
  {19, 2},
  {23, 2},
  {28, 2},
  {29, 2},
  {17, 3},
  {28, 3},
  {29, 3},
  {3, 4},
  {4, 4},
  {3, 5},
  {4, 5},
  {6, 5},
  {8, 5},
  {17, 5},
  {19, 5},
  {23, 5},
  {28, 5},
  {29, 5},
  {4, 7},
  {26, 7},
  {28, 7},
  {17, 8},
  {28, 8},
  {29, 8},
  {6, 9},
  {17, 9},
  {23, 9},
  {28, 9},
  {29, 9},
  {17, 10},
  {29, 10}
};
static const uint8_t al_wildcard_errors[] = { 1 };
static const al_state_policy_t al_state_policies[] = {
  {3, 12, 3},
  {4, 12, 3},
  {6, 12, 3},
  {8, 12, 3},
  {23, 12, 3},
  {26, 12, 3},
  {17, 7, 5},
  {19, 12, 3},
  {28, 12, 3},
  {5, 15, 15},
  {7, 8, 11},
  {30, 15, 15},
  {29, 12, 11}
};
static const uint8_t al_generic_idempotent_funcs[] = { 6, 23, 17, 29 };

const al_request_contract_t *al_find_request_contract(uint8_t func, uint8_t op)
{
  size_t i;
  for (i = 0u; i < al_request_contract_count; ++i) {
    if (al_request_contracts[i].func == func &&
        (func != 28u || al_request_contracts[i].op == op))
      return &al_request_contracts[i];
  }
  return NULL;
}

int al_response_func_allowed(uint8_t request_func, uint8_t op,
                             uint8_t response_func)
{
  const al_request_contract_t *request =
      al_find_request_contract(request_func, op);
  size_t index;
  if (request == NULL) return 0;
  index = (size_t)(request - al_request_contracts);
  if (al_response_contracts[index].normal_func == response_func) return 1;
  return al_response_contracts[index].allow_exception &&
         response_func == (uint8_t)(0x80u | request_func);
}

int al_response_payload_allowed(uint8_t f, uint8_t op, const uint8_t *q,
                                uint16_t qn, uint8_t addr, const uint8_t *p,
                                uint16_t n)
{
  uint16_t pos;
  uint16_t i;
  uint16_t j;
  (void)op;
  if ((n != 0u && p == NULL) || (qn != 0u && q == NULL)) return 0;
  if (f == 3u) {
    if (n < 5u || !gs_sensor_type_valid(p[0]) ||
        (p[n - 1u] & ~7u) != 0u) return 0;
    pos = 4u;
    for (i = 0u; i < p[1]; ++i) {
      if (pos + 2u > n - 1u || p[pos] == 0u || p[pos + 1u] == 0u ||
          pos + 2u + p[pos + 1u] > n - 1u) return 0;
      for (j = 4u; j < pos; j = (uint16_t)(j + 2u + p[j + 1u]))
        if (p[j] == p[pos]) return 0;
      pos = (uint16_t)(pos + 2u + p[pos + 1u]);
    }
    return pos == n - 1u;
  }
  if (f == 4u) {
    if (qn != 1u) return 0;
    if (q[0] == 0u)
      return n == 15u && gs_sensor_type_valid(p[8]) &&
             p[9] == addr && gs_capabilities_valid(p[10]) &&
             p[11] == 2u && p[12] == 0u;
    if (q[0] == 1u) {
      if (n < 4u || !gs_sensor_type_valid(p[0])) return 0;
      pos = 4u;
      i = 0u;
      while (pos < n) {
        uint8_t type;
        uint8_t length;
        if (pos + 2u > n) return 0;
        type = p[pos];
        length = p[pos + 1u];
        if (pos + 2u + length > n) return 0;
        if (type == 1u) {
          if (length != 6u || p[pos + 2u] == 0u ||
              !gs_quantity_id_valid(p[pos + 3u]) ||
              !gs_data_type_valid(p[pos + 4u]) ||
              !gs_unit_id_valid(p[pos + 5u]) ||
              !gs_descriptor_flags_valid(p[pos + 7u])) return 0;
          ++i;
        }
        pos = (uint16_t)(pos + 2u + length);
      }
      return pos == n && i == p[1];
    }
    return 0;
  }
  if (f == 6u)
    return qn >= 1u && n == 2u && p[0] == q[0] && p[1] <= 3u;
  if (f == 8u)
    return n == qn && (n == 0u || memcmp(p, q, n) == 0);
  if (f == 23u) return n == 1u && p[0] == 0u;
  if (f == 19u) return n == 1u && p[0] == addr;
  if (f == 28u)
    return n == 2u && ((p[0] == 0u && p[1] == 0u) ||
                      (p[0] == 1u && p[1] >= 1u && p[1] <= 3u));
  if (f == 26u) return 0;
  return 1;
}

int al_is_known_request(uint8_t func)
{
  size_t i;
  for (i = 0u; i < al_request_contract_count; ++i)
    if (al_request_contracts[i].func == func) return 1;
  return 0;
}

int al_request_length_allowed(uint8_t func, const uint8_t *data,
                              uint16_t length)
{
  const al_request_contract_t *request;
  uint8_t op = 0u;
  if (func == 28u) {
    if (length < 10u || data == NULL) return 0;
    op = data[8];
    if (op != 1u && op != 3u &&
        op != 2u) return 1;
  }
  request = al_find_request_contract(func, op);
  if (request == NULL) return 0;
  if (request->length_kind == AL_LEN_FIXED) return length == request->length;
  if (request->length_kind == AL_LEN_RANGE) return length <= 247u;
  if (length < 1u || data == NULL) return 0;
  return al_config_request_length(data[0]) == 0u ||
         al_config_request_length(data[0]) == length;
}

int al_node_state_allowed(uint8_t func, uint8_t commission, uint8_t service)
{
  size_t i;
  if (commission >= 4u || service >= 4u)
    return 0;
  for (i = 0u; i < sizeof(al_state_policies) / sizeof(al_state_policies[0]); ++i) {
    if (al_state_policies[i].func == func)
      return (al_state_policies[i].commission_mask & (1u << commission)) != 0u &&
             (al_state_policies[i].service_mask & (1u << service)) != 0u;
  }
  return 0;
}

int al_is_generic_idempotent(uint8_t func)
{
  size_t i;
  for (i = 0u; i < sizeof(al_generic_idempotent_funcs); ++i)
    if (al_generic_idempotent_funcs[i] == func) return 1;
  return 0;
}

int al_candidate_confirmation_eligible(uint8_t func, uint8_t op)
{
  const al_request_contract_t *contract = al_find_request_contract(func, op);
  return contract != NULL && contract->txn == AL_TXN_UNICAST &&
         contract->address_role == AL_ADDR_CURRENT;
}

int al_is_wire_error(uint8_t code)
{
  static const uint8_t values[] = { 1, 2, 3, 4, 5, 7, 8, 9, 10 };
  size_t i;
  for (i = 0u; i < sizeof(values); ++i)
    if (values[i] == code) return 1;
  return 0;
}

int al_error_allowed(uint8_t request_func, uint8_t code)
{
  size_t i;
  for (i = 0u; i < sizeof(al_wildcard_errors); ++i)
    if (al_wildcard_errors[i] == code) return 1;
  for (i = 0u; i < sizeof(al_error_pairs) / sizeof(al_error_pairs[0]); ++i)
    if (al_error_pairs[i].func == request_func &&
        al_error_pairs[i].code == code) return 1;
  return 0;
}

uint8_t al_config_request_length(uint8_t config_id)
{
  switch (config_id) {
  case 1u: return 5u;
  case 2u: return 3u;
  case 3u: return 3u;
  case 4u: return 3u;
  default: return 0u;
  }
}

uint8_t al_data_type_width(uint8_t data_type)
{
  switch (data_type) {
  case 1u: return 2u;
  case 2u: return 2u;
  case 3u: return 4u;
  default: return 0u;
  }
}

int al_is_supported_baud(uint32_t value)
{
  static const uint32_t values[] = { 9600u, 19200u, 57600u, 115200u };
  size_t i;
  for (i = 0u; i < sizeof(values) / sizeof(values[0]); ++i)
    if (values[i] == value) return 1;
  return 0;
}

size_t al_supported_baud_count(void)
{
  static const uint32_t values[] = {9600u, 19200u, 57600u, 115200u};
  return sizeof(values) / sizeof(values[0]);
}

uint32_t al_supported_baud_at(size_t index)
{
  static const uint32_t values[] = {9600u, 19200u, 57600u, 115200u};
  return index < sizeof(values) / sizeof(values[0]) ? values[index] : 0u;
}
