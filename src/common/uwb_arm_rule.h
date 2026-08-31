#ifndef UWB_ARM_RULE_H
#define UWB_ARM_RULE_H

#include <stdint.h>

/* Trigger rule: chain of conditions linked by AND/OR, evaluated in
 * disjunctive normal form (AND binds tighter than OR). Each condition
 * must hold continuously for hold_ms before it counts as satisfied;
 * an AND group fires only when all its conditions are satisfied
 * simultaneously. */

#define UWB_ARM_RULE_MAX_CONDS 8u

#define UWB_ARM_SRC_DIST 0u
#define UWB_ARM_SRC_TILT 1u

#define UWB_ARM_OP_GT 0u
#define UWB_ARM_OP_LT 1u

/* Link between this condition and the NEXT one. */
#define UWB_ARM_LINK_END 0u
#define UWB_ARM_LINK_AND 1u
#define UWB_ARM_LINK_OR  2u

typedef struct
{
    uint8_t  source;          /* UWB_ARM_SRC_* */
    uint8_t  op;              /* UWB_ARM_OP_* */
    uint8_t  link;            /* UWB_ARM_LINK_* (link to next condition) */
    uint8_t  reserved;
    int32_t  threshold_milli; /* DIST: millimetres, TILT: millidegrees from vertical */
    uint32_t hold_ms;         /* condition must stay true this long (0 = immediate) */
} uwb_arm_cond_t;

typedef struct
{
    uint32_t       count;     /* 0 = no rule (disarmed) */
    uwb_arm_cond_t conds[UWB_ARM_RULE_MAX_CONDS];
} uwb_arm_rule_t;

#endif /* UWB_ARM_RULE_H */
