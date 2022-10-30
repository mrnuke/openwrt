# Realtek switch support to do list

This file intends to keep track of known issues and shortcomings with current
downstream OpenWrt driver. These drivers are of insufficient quality, or the
knowledge of the hardware they control has too many gaps, to allow for these
drivers to be submitted upstream.

The action items are sorted per driver/peripheral, roughly ordered by
importance. Every driver should have a listed developer or contact, usually the
original driver author.


## Switch core support

### Switch port LED driver
Sander Vanheule <sander@svanheule.net>

* Changing the HW offloading state will affect both the primary and secondary
  LEDs of a port. This is currently not propagated, leaving the sibbling LED
  with an incorrect trigger value.
* Reading the LED brightness will always return the user-controlled brightness
  setting. When HW offloading is active, this value may be incorrect.
