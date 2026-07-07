The remaining divergence between my fork and upstream main is pump speed and flow control. The building blocks to do this correctly seem to be in the code (`set_constant_flow_async` and the other setpoint methods, and `send_control_request` takes a real value), but as it is currently wired the controls do not behave correctly.

## Control problems

Speed control settings not working properly:

- **Enabling the pump uses a hard-coded default instead of the setpoint.** In constant-speed mode, turning Pump Enabled "on" runs a fixed ~3674 RPM regardless of the constant speed setpoint. That value sent with the command to turn the pump on is the default suffix `{0x45, 0x65, 0x70, 0x00}` from the mode's row in `CLASS10_CONTROL_MAP`. Each mode has its own row, but the pressure, speed, and flow rows all hold that same default suffix (the constant-flow row is even commented "suffix same as pressure"), so none of them send the actual setpoint on enable.  Interestingly, setting the constant-speed setpoint after the pump is already running does change the speed of the pump to that setpoint.
- **Constant flow rate also just runs the pump at the same 3674 RPM.** When I tried constant flow rate mode, it just kept running at 3674 RPM no matter what I set the desired flow rate to and also goes to that same number when I enable the pump.  Unlike constant speed mode, when in constant flow rate mode, changing the constant flow setpoint does not seem to change the speed of the pump.  It just runs at the same ~3674 rpm.
- **Enable and the on/off entities desync.** Changing the setpoint for either constant speed or constant flow rate when the pump is off, turns the pump on. Pump Motor Active reads on, but Remote Mode and Pump Enabled both read off. The component's on/off state gets out of sync with the pump, and I have to toggle Pump Enabled on and then off to turn the pump off.
- **Changing the Pump Mode turns the pump on** If the pump was off and you are just trying to configure the pump, but not turn it on, this will surprise you as it turns the pump on (not a serious problem because this is typically a one-time configuration).  But, unlike with the flow rate adjustments, this keeps the Pump Enabled and Remote Mode properly synced.  So, if the pump is off and you change the Pump Mode (say from constant flow to constant speed), the pump will turn on and the Pump Enabled and Remote Mode will properly show as on.

## Where these behaviors come from

This is what I traced, locations only, not proposing a fix:

- **Enable drops the setpoint.** Pump Enabled's turn-on calls `pump_start()`, which calls `start()`, which calls `send_control_request(mode, true)` with no setpoint (the third arg defaults to `NAN`), so it falls back to the `CLASS10_CONTROL_MAP` default suffix. The configured setpoint is never sent on enable.
- **A setpoint change forces enable.** The setpoint numbers call `set_constant_speed(x)` / `set_constant_flow(x)`, which call `send_control_request(mode, true, x)`. The middle `true` is the enable flag, so writing a setpoint also turns the pump on.
- **The on/off entities are not reconciled with the pump.** `pump_enabled_` is set by `start()` and `stop()` and re-derived from pump notifications, but the setpoint path (`set_*` calling `send_control_request`) never sets it, so the switch lags a setpoint-triggered start until a notification catches up. `is_remote_mode_enabled_` is set only by `enable_remote` and `disable_remote`; the pump's actual control-source (decoded from notifications) is never written back, so Remote Mode reflects the last command rather than the pump's real state.

## Design Note

There's a bit of a conflict in how this works.  If the setpoint is supposed to be set separately and is supposed to be stored in the pump as the source of truth, then on startup, it's awkward to have to get the setpoint from the pump so you can then send it back to the pump with the Enabled command in order to get the pump to actually run at the setpoint speed.  It seems there could be a window upon startup for the component to not yet know what the setpoint is, yet it's being asked to enable the pump.  There could also be a window for the local value of the setpoint to be out of sync with the pump during any transition while it's being changed.  The model of storing the setpoint in the pump would be be a better match to a method of turning on the pump that didn't require sending the speed with the command.  

If there was a command (perhaps a Class 3 command) to turn the pump on and off without sending a speed (so it would rely on the setpoint the pump already holds), that would decouple the enable control from setpoint for turning the pump on and then the setpoint in the pump could just do its job.

## The two usage models

There are at least two usage models for the pump, and Grundfos drew a line between them with Remote Mode:

- **Pump-driven** (Remote Mode off): you configure setpoints (or an auto mode), maybe a schedule, and the pump turns itself on and off as configured. This configuration (including any neccesary setpoints) have to live in the pump, since it needs them the next time its schedule fires and it turns itself on.
- **External control** (Remote Mode on, what I use): something outside the pump decides when it runs and at what speed or flow.  This model would not need to rely on data stored in the pump as it could just set what it wants when it wants to turn on the pump.

In my case an HA pyscript automation decides pump on/off based on temperature sensors, schedule, presence, vacation, and so on. I'm trying to configure the constant speed (or constant flow) setpoint once and then never change it because that's how it looks like the component is designed to function, but it isn't working that way because just turning the pump on whacks the speed the pump is running at to something other than the setpoint.

I do not have a proposal for the cleanest way to support both models. I am mainly surfacing the control defects above and sharing the operator's-eye view of the two models, since I have been living in the external-control one.  If the model where setpoints are reliably set in the pump and retained in the pump and are not disturbed by turning the pump on or off via external control, I could live with that.  But, it seems fraught with some difficulties because if the component first boots up an the client wants to immediately turn the pump on, as long as the component is using a command that also requires the speed be sent, then the component must have retrieved the desired setpoint from the pump before sending it back to the pump (which all seems a bit backwards).  The next section describes my work-around which uses an HA control to hold the desired speed setpoint rather than getting it from the pump (see below).

## How I work around speed control issues now ##

Pump speed was one of the first things I hit trying to use this component. Not knowing the code, I first just edited the hard-coded value in the `CLASS10_CONTROL_MAP` constant-speed row and ran with that (and that worked). When I wanted to actually vary the speed, I added my own HA-hosted "Target Recirc Speed" entity and, instead of pulling the speed from the table, I read it from that entity and send it with the Class 10 turn-on command. That has been working for me, so I am effectively bypassing the pump setpoint entirely. Since the Class 10 turn-on command carries a speed anyway, I just send whatever my control is set to each time I turn the pump on. This is just how I have coped, not a proposed fix, though this has some advantages for me in that my speed setting is in HA where it survives pump replacement or pump reset.

## Notes

There appears to be a Class 3 command to turn the pump on and off without setting a speed (so it would rely on the setpoint the pump already holds), which would decouple the enable control from setpoint for turning the pump on. I do not know what ALPHA HWR pumps support it; that could be checked with some test code on the pumps we have.

