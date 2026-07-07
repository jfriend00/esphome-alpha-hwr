The remaining divergence between my fork and upstream main is pump speed and flow control. The building blocks to do this correctly seem to be in the code (`set_constant_flow_async` and the other setpoint methods, and `send_control_request` takes a real value), but as it is currently wired the controls do not behave correctly.  There is a lot written here, because as I started diving into the control problems I found and was looking for possible solutions, I uncovered a bunch to share and some of the options are more than just fixing bugs (design decisions to be made) so I thought I'd just dump what I know and we can discuss from there when you have time to digest everything.

# Control Problems

First, I'll document the problems I found.  Then, in a separate section that follows, I offer commentary on each issue and what I've learned about them that will hopefully aid in fixing the control issues.

### Enabling the pump changes the pump speed to a hard-coded speed (~3674 RPM) instead of allowing the pump to use the setpoint

In constant-speed mode, turning Pump Enabled "on" runs a fixed ~3674 RPM regardless of the constant speed setpoint. That value sent with the command to turn the pump on is the default suffix `{0x45, 0x65, 0x70, 0x00}` from the mode's row in `CLASS10_CONTROL_MAP`. Each mode has its own row, but the pressure, speed, and flow rows all hold that same default suffix (the constant-flow row is even commented "suffix same as pressure"), so none of them send the actual setpoint on enable.  Interestingly, setting the constant-speed setpoint after the pump is already running does change the speed of the pump to that setpoint.

### Constant flow rate also just runs the pump at the same 3674 RPM

When I tried constant flow rate mode, it just kept running at 3674 RPM no matter what I set the desired flow rate to and also goes to that same number when I enable the pump.  Unlike constant speed mode, when in constant flow rate mode, changing the constant flow setpoint does not seem to change the speed of the pump in my tests so it appears the constant flow setpoint has issues also.  FYI, these sliders for speed and flow rate are difficult to use because of the non-optimistic settings.  I understand the purpose of being non-optimistic, but they echo back of the actual set state seems pretty slow for these settings and leads to some confusion in the UI.  I'm not sure what the best solution is for that.  There also appears to be some sort of units or scaling problem with constant flow.  If I switch the pump mode to Constant Flow, I will see Constant Flow Setpoint = 0.003056 gal/min and Flow Rate = 3.940 gal/min and Motor Speed = 3670 RPM.  So, clearly the setpoint is not displaying correctly.

### Enable and the on/off entities desync

Changing the setpoint for either constant speed or constant flow rate when the pump is off, turns the pump on. Pump Motor Active reads on, but Remote Mode and Pump Enabled both read off. The component's on/off state gets out of sync with the pump, and I have to toggle Pump Enabled on and then off to turn the pump off.  If an automation or a user is looking at the Pump Enabled setting to judge current state (which mine does, this will lead to confusion).  FWIW, the motor speed readout (zero or some non-zero value) is accurately reflected either way (so I may change my automation to look at that to judge pump state though it has significant lag because it comes from telemetry).

### Changing the Pump Mode turns the pump on

If the pump was off and you are just trying to configure the pump, but not turn it on, this will surprise you as changing the pump mode turns the pump on (not a serious problem because this is typically a one-time configuration for most uses).  But, unlike with the flow rate adjustments, this keeps the Pump Enabled and Remote Mode properly synced.  So, if the pump is off and you change the Pump Mode (say from constant flow to constant speed), the pump will turn on and the Pump Enabled and Remote Mode will properly show as on.

### The existing Remote Mode is broken (wrong operation code), and remote is not required to command the pump

While bench-testing on my pump I found that `enable_remote_mode()` and `disable_remote_mode()` do not actually do anything. They build the Class 3 frame with the wrong operation code.  This accidentally confirms that remote mode does not appear to be required to send commands to the pump.

# Learnings and Discussion of the Control Problems

### Pump Enable does not respect the setpoint for either mode I tested. 
    
Pump Enabled's turn-on calls `pump_start()`, which calls `start()`, which calls `send_control_request(mode, true)` with no setpoint (the third arg defaults to `NAN`), so it falls back to the `CLASS10_CONTROL_MAP` default suffix. The configured setpoint is never sent on enable.  The default value from the `CLASS10_CONTROL_MAP` is always used and thus the same default pump speed is always set whenever you enable the pump.

One solution would be to get the current setpoint for the current operating mode and be sure to send it with the class 10 command to turn on the pump.  But, this has potential issues because the component has to make sure it's always in sync with what is stored in the pump for the setpoint.  This could create timing problems at startup or when a setpoint had been recently modified.  Plus, it feels a bit odd to put a setpoint in the pump and not really use it.  At startup, if an automation using the component wants to turn the pump on right away, the component would have to first get the setpoint from the pump, then put it in the Class 10 command to turn on the pump just so it can send it right back to the the pump. 

Another solution is to not rely on the setpoint in the pump at all and store the desired speed in an HA entity so the speed can just be used from that and sent with the start command.  This is my current work-around that I've been using for awhile.  I chose this in my early days with the component just to get up and running with speed control with the least modification of those code and without have to do all the research that went into this doc.  While this works for my usage model of 100% remote control, it would not work for automatic modes with remote mode off (like where the pump runs off its internal schedule) where the setpoint must be in the pump.

There is an alternative with a Class 3 command (see next point) that turns on/off the pump and does not contain any info about the desired speed and I've tested these commands on my pump.  They work for turning the pump on/off and it respects whatever setpoint is already in the pump.

### Class 3 START and STOP: a clean on/off that respects the pump's stored setpoint**

Class 3 commands are issued with the SET operation, which is `0x81` in the second APDU byte (operation 2 in the top two bits, length 1 in the low six). With that, the command set gives a bare on/off that does not touch the setpoint. This is the piece that decouples on/off from the setpoint write.

**Frame format** (same GENI envelope as the existing remote-mode commands):

- APDU = `[0x03, 0x81, <command_id>]`
    - `0x03` = Class 3 (Commands)
    - `0x81` = SET operation (2) in the top two bits, length 1 in the low six
    - `<command_id>` = a single byte
- Command IDs: **START = 0x06**, **STOP = 0x05** (from the GENIbus command class: STOP 5, START 6, REMOTE 7, LOCAL 8)

**Behavior confirmed on the pump:**

- **START (0x06)** turns the pump on and runs it at the setpoint already stored in the pump. It sends no setpoint and needs none. I verified this by setting the in-pump setpoint to 900 RPM (below the pump's floor), stopping, then sending START. The pump came up at about 1654 RPM, which is its minimum, showing it used the stored value rather than any default.
- **STOP (0x05)** turns the pump off.
- I confirmed both by reading operation mode back from the pump: after START it reads 0 (running), after STOP it reads 1 (stopped).

**Reply format:** a successful SET command returns an 8-byte frame such as `24 04 F8 E7 03 00 0D 89`, whose APDU is `[03 00]`: class 3, ack OK, zero data.

**Implementation note:** a Class 3 command does not produce an unsolicited notification from the pump. So cached state, and therefore any non-optimistic Home Assistant entity such as the Pump Enabled switch, will not update on its own after a START or STOP. To keep state in sync, do a `get_mode` read-back shortly after sending. I used a 500 ms delay and it works reliably.  There's more on this below.

### A setpoint change forces pump enable

The setpoint functions call `set_constant_speed(x)` / `set_constant_flow(x)`, which call `send_control_request(mode, true, x)`. The middle `true` is the enable flag, so writing a setpoint also turns the pump on.  I do not know if there's a way to change the setpoint without specifying the on/off state and haven't fully investigated that.  A work-around that I have tested is to send the current on/off state with the current command (instead of the hardwired "on") so that the on/off state doesn't change.  Note, this command uses a fused control object that contains pump mode, setpoint for that mode and the on/off state. I've verified that if the pump is off and the constant speed setpoint is at 2000 rpm and you call `send_control_request()` with constant speed, false and 3000 that the pump stays off and speed setpoint is modified to 3000 rpm.  If I then issue a Class 3 Start command (to only turn the pump on and not influence anything else), it will then operate at the new setpoint of ~3000rpm.  So, just including the current state of the pump in these commands is a workable solution to keep from turning the pump on when just adjusting the settings.  Note these commands also change the pump mode also (that's just how they work) which can occasionally create a little UI confusion if you think you're just adjusting the setpoint for a different mode than you are currently using.  This isn't a real world problem for me (and probably most users) because I pick an operating mode and stay in it.  I don't jump around between operating modes except when I'm exploring what the options are in the pump and how they work. 

### The on/off entities are not reconciled with the pump

`pump_enabled_` is set by `start()` and `stop()` and re-derived from pump notifications, but the setpoint path (`set_*` calling `send_control_request`) never sets it, so the switch lags a setpoint-triggered start until a notification catches up. `is_remote_mode_enabled_` is set only by `enable_remote` and `disable_remote`; the pump's actual control-source (decoded from notifications) is never written back, so Remote Mode reflects the last command rather than the pump's real state.

If the setpoint is fixed to not change the on/off state of the pump, then this problem probably goes away.

### Remote Mode broken

The current implementation is sending the INFO operation, not the SET operation.  The second APDU byte packs the operation in the top 2 bits and the length in the low 6 bits. The current code uses `0xC1`, which is operation 3 (INFO) with length 1. INFO only asks the pump to describe the data item; it never executes it. To execute a command you need operation 2 (SET), which for a one-byte command is 0x81.

The pump's own replies show it plainly. The current `[03 C1 07]` comes back as `[03 01 AC]`, which is class 3, ack OK, one data byte (the INFO descriptor). Sending `[03 81 07]` instead comes back as [03 00], which is class 3, ack OK, zero data, a clean execution acknowledgement.

Two things worth knowing about the impact:

First, this has not caused an obvious problem for basic control, because being in remote mode is not required for the pump to accept external commands. I can start and stop the pump with the control source reading local (0) the entire time. So the broken toggle has effectively been invisible for on/off.

Second, whatever remote mode is actually for (most likely telling the pump to stand down its own automatic behavior, such as an internal schedule) is perhaps not happening (would need testing to see if this pump even alters its behavior when Remote Mode is set and if Remote mode stays on). In my HA automation, I specifically turn off the pump scheduling feature in HA with `switch.recirc_controller_schedule_enabled` to avoid it ever interfering with my automation's control.

The fix is a one-byte change in each Class 3 command: use 0x81 (SET) instead of 0xC1 (INFO) in the second APDU byte.  

It's up to you if you want to actually fix this to turn this mode on and off or remove it.  On the one hand, Remote Mode wasn't actually getting engaged and we haven't seen any consequences of it not working (so maybe remove it since we didn't appear to need it).  Also, there are unknowns with actually turning Remote Mode on now.  Perhaps some behaviors that we currently rely on get changes.

The doc you can find on this topic makes it sound mandatory (which it apparently isn't for our pump because our commands work fine without properly engaging Remote Mode), but it also sounds like it's very much about industrial devices on an RS-485 bus which just may not apply to our pumps.  I might personally lean to the "if it ain't broke, don't fix it" side of things (perhaps even removing it from the component) until I have a concrete reason to think it needs to be implemented and used.  One place to go looking for an effect is whether built-in pump scheduling that is enabled is suspended automatically when Remote Mode is on vs off and whether Remote Mode actually stays on after you turn it on (the doc refers to an auto-off mode after 6 seconds of if the bus is not still addressing the device).  If Remote Mode does anything on our pump and if we enagage it and if it has an auto shut-off, there's a chance that our constant telemetry engagement is a keep-alive for Remote Mode to avoid the auto shut-off.  I've not attempted to test the scheduling feature in this way.

### The Remote Mode switch reflects a local intent flag, not the pump's state

The Remote Mode switch is declared non-optimistic (`optimistic: false`), which implies it reads the pump's actual state, but its lambda returns `get_remote_enabled()`, which just returns a local cached flag (`is_remote_mode_enabled_`). That flag is set true inside `enable_remote_mode()` and cleared only inside `disable_remote_mode()`. Since enabling the pump calls `enable_remote()` before starting, the switch latches ON the first time you enable the pump and stays on until you manually toggle it off. It reflects the component's intent, not the pump.

Meanwhile the pump's actual control-source byte (read in `get_mode`) is only logged and then discarded, so it never feeds the switch. And in my testing that byte reads 0 in every capture (46 readings across 7 sessions) and never responds to REMOTE or LOCAL.

So the switch is disconnected from the pump's real remote/local state on two counts: it is driven by a latching local flag rather than a readback, and even the readback we do have does not appear to track remote/local. Combined with the operation-code bug (the enable command is INFO, so it does nothing on the pump anyway), the switch currently shows "remote enabled" for a command that had no effect.

Making it trustworthy would mean deciding what the switch should represent and finding which field actually reports remote state on the ALPHA, since `control_source` does not seem to.

### The pump does not self-report state changes after a command

When a command changes the pump's state, the pump does not send back an unsolicited notification, at least not for the Class 3 start/stop commands I tested. So a non-optimistic entity that mirrors cached pump state, such as the Pump Enabled switch, will not update on its own after such a command. I saw this directly: the switch only began following C3 start/stop once I added a `get_mode` read-back right after sending.  I also saw this with a Class 10 command when changing the setpoint with the pump off which caused the pump to be turned on, but that new state was not represented in the Pump Enabled control.

From the logs it also looks like the component reads the full control state (`get_mode`) only at startup, not on a periodic basis, so the cache reflects what the component last commanded rather than the pump's live state. That is fine as long as every state change comes through a command path that updates the cache, but it means any out-of-band change (a Class 3 command, or the pump acting on its own) will not show up until the next boot.

If you adopt the Class 3 start/stop commands, the simplest fix is a `get_mode` read-back shortly after sending (I used about 500 ms). A periodic `get_mode` poll would cover the more general case of the pump changing state on its own.


# How I work around speed control issues now ##

Pump speed was one of the first things I hit trying to use this component. Not knowing the code, I first just edited the hard-coded value in the `CLASS10_CONTROL_MAP` constant-speed row and ran with that (and that worked). When I wanted to actually vary the speed, I added my own HA-hosted "Target Recirc Speed" entity and, instead of pulling the speed from the table, I read it from that entity and send it with the Class 10 turn-on command. That has been working for me, so I am effectively bypassing the pump setpoint entirely. Since the Class 10 turn-on command carries a speed anyway, I just send whatever my control is set to each time I turn the pump on. This is just how I have coped, not a proposed fix, though this has some advantages for me in that my speed setting is in HA where it survives pump replacement or pump reset.


# References

The external source that made the Class 3 operation encoding clear (including the SET command) is christoph2's GENIbus library, which reverse-engineers the Grundfos GENIbus protocol: https://github.com/christoph2/GENIBus

The relevant pieces:

- https://github.com/christoph2/GENIBus/src/genibus/gbdefs.py defines the operation codes (GET = 0, SET = 2, INFO = 3) and the acknowledge codes (OK = 0, plus the error codes). This is where you can confirm that 0xC1 decodes to INFO and 0x81 to SET.
- https://github.com/christoph2/GENIBus/src/genibus/apdu.py shows how the second APDU byte is assembled: (operationSpecifier << 6) | (length & 0x3F), so operation in the top 2 bits and length in the low 6. It also has createSetCommandsAPDU(), which issues a command by building the frame with APDUClass.COMMANDS (3) and Operation.SET. For a single command that produces [3, 0x81, command_id], which is exactly the frame that works on my pump.
- https://github.com/christoph2/GENIBus/src/genibus/devices/upe.json (the UPE circulator data dictionary, the closest family to the ALPHA) lists the Class 3 command IDs with descriptions, including STOP = 5 "Stops the pump", START = 6 "Starts the pump", REMOTE = 7, LOCAL = 8.

One internal cross-check in your own code: the Class 10 control write already uses SET. In control_service.cpp the control-request APDU sets its second byte to 0x90, with the comment "OpSpec: SET with length 16". 0x90 is SET (2) in the top 2 bits with length 16. So the Class 3 commands just need that same SET operation; they are the only ones currently using INFO.