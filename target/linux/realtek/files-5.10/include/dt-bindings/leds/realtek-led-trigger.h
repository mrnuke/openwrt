/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2022 Sander Vanheule
 *
 * Realtek switch port trigger flags
 */
#ifndef __DT_BINDINGS_LEDS_REALTEK_LED_TRIGGER_H
#define __DT_BINDINGS_LEDS_REALTEK_LED_TRIGGER_H

#define RTL_LED_NONE			0
#define RTL_LED_ACT_RX			(1 << 0)
#define RTL_LED_ACT_TX			(1 << 1)
#define RTL_LED_LINK_10			(1 << 2)
#define RTL_LED_LINK_100		(1 << 3)
#define RTL_LED_LINK_1000		(1 << 4)
#define RTL_LED_LINK_2500		(1 << 5)
#define RTL_LED_LINK_5000		(1 << 6)
#define RTL_LED_LINK_10000		(1 << 7)

/* Frequently used combinations */
#define RTL_LED_ACT			(RTL_LED_ACT_RX | RTL_LED_ACT_TX)
#define RTL_LED_LINK_10_100		(RTL_LED_LINK_10 | RTL_LED_LINK_100)
#define RTL_LED_LINK_10_100_1000	(RTL_LED_LINK_10 | RTL_LED_LINK_100 | RTL_LED_LINK_1000)

#endif /* __DT_BINDINGS_LEDS_REALTEK_LED_TRIGGER_H */
