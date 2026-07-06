The remaining divergence between my fork and upstream main is pump speed and flow control. The building blocks to do this correctly seem to be in the code (`set_constant_flow_async` and the other setpoint methods, and `send_control_request` takes a real value), but as it is currently wired the controls do not behave correctly. Here is what I see, then what I saw in the code, then some context on the two usage models.

## Control problems

Speed control settings not working properly:

- **Enabling the pump uses a hard-coded default instead of the setpoint.** In constant-speed mode, turning Pump Enabled on runs a fixed ~3674 RPM regardless of the constant speed setpoint. That value is the default suffix `{0x45, 0x65, 0x70, 0x00}` from the mode's row in `CLASS10_CONTROL_MAP`. Each mode has its own row, but the pressure, speed, and flow rows all hold that same default suffix (the constant-flow row is even commented "suffix same as pressure"), so none of them send the actual setpoint on enable.
- **Constant flow rate is unusable.** When I tried constant flow rate mode, it just kept running at 3674 RPM no matter what I set the desired flow rate to.
- **Enable and the on/off entities desync.** Changing the setpoint turns the pump on (see below). Pump Motor Active reads on, but Remote Mode and Pump Enabled can both read off. The component's on/off state gets out of sync with the pump, and I have to toggle Pump Enabled on and then off to resync it.

Undesirable side-effect:

- **Setting a setpoint always turns the pump on.** The Class 10 command that writes a setpoint also enables the pump, and the setpoint methods pass enable=true, so you cannot change a setpoint while the pump is off without it turning on. This holds for constant speed and constant flow, and by code path presumably the pressure and proportional modes I have not tested.

## Where these behaviors come from

As far as I traced, locations only, not proposing the fix:

- **Enable drops the setpoint.** Pump Enabled's turn-on calls `pump_start()`, which calls `start()`, which calls `send_control_request(mode, true)` with no setpoint (the third arg defaults to `NAN`), so it falls back to the `CLASS10_CONTROL_MAP` default suffix. The configured setpoint is never sent on enable.
- **A setpoint change forces enable.** The setpoint numbers call `set_constant_speed(x)` / `set_constant_flow(x)`, which call `send_control_request(mode, true, x)`. The middle `true` is the enable flag, so writing a setpoint also turns the pump on.
- **The on/off entities are not reconciled with the pump.** `pump_enabled_` is set by `start()` and `stop()` and re-derived from pump notifications, but the setpoint path (`set_*` calling `send_control_request`) never sets it, so the switch lags a setpoint-triggered start until a notification catches up. `is_remote_mode_enabled_` is set only by `enable_remote` and `disable_remote`; the pump's actual control-source (decoded from notifications) is never written back, so Remote Mode reflects the last command rather than the pump's real state.

## The two usage models

There are two ways to use the pump, and Grundfos already drew the line between them with Remote Mode:

- **Pump-driven** (Remote Mode off): you configure setpoints (or an auto mode) plus a schedule, and the pump turns itself on and off. Setpoints have to live in the pump, since it needs them the next time its schedule fires.
- **External control** (Remote Mode on, what I use): something outside the pump decides when it runs and at what speed or flow.

In my case an HA pyscript automation decides on/off and speed from temperature, schedule, presence, vacation, and so on. For that model, keeping the setpoints in HA rather than the pump is actually an advantage: they survive a pump reset or replacement, so a new pump only needs pairing. And in external control the setpoint arguably does not need to live in the pump at all, since the Class 10 enable command carries the value every time.

That split is roughly what is behind the current behavior: the Class 10 enable-with-setpoint command suits external control (send the value each time you turn it on), but fusing setpoint-writing with enabling is awkward for the pump-driven model.

I do not have a proposal for the cleanest way to support both models. I am mainly surfacing the control defects above and sharing the operator's-eye view of the two models, since I have been living in the external-control one.

## How I work around speed control issues now ##

Pump speed was one of the first things I hit trying to use this component. Not knowing the code, I first just edited the hard-coded value in the `CLASS10_CONTROL_MAP` constant-speed row and ran with that (and that worked). When I wanted to actually vary the speed, I added my own HA-hosted Target Recirc Speed entity and, instead of pulling the speed from the table, I read it from that entity and send it with the Class 10 turn-on command. That has been working for me, so I am effectively bypassing the pump setpoint entirely. Since the Class 10 turn-on command carries a speed anyway, I just send whatever my control wants each time I turn the pump on. This is just how I have coped, not a proposed fix.

## Notes

There appears to be a Class 3 command to turn the pump on and off without setting a speed (so it would rely on the setpoint the pump already holds), which would decouple the enable control from setpoint for the pump-driven model. I do not know what ALPHA HWR pumps support it; that could be checked with some test code on the pumps we have.

