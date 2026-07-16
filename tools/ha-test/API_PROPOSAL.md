This is a design discussion rather than a bug report, and it runs a bit long. It comes in three parts: the problem, why it is structural rather than a set of bugs, and a proposed direction. One thing up front: this is not a request to tune a particular delay. Every time we pin down one timing value, the next client just hits the next one.

# The Problem

## Background

The impetus for writing this document comes out of things I've learned when building the test harness discussed in #63. That tool saves and restores the pump's state through the Home Assistant entities and the idea is to set and verify various values as tests: it writes a mode, the setpoints for that mode, and the on/off state, and then it verifies that each value actually took. Verifying writes is a core part of its job, not an extra.

The test tool has been a timing nightmare. There are multiple complications involved in writing to different types of entities. When trying to write sequences of settings that include a mode change and/or a pump enabled change, I would regularly get overwrites. In one case, the pump speed had been set to 2000; I set it to 1650, then turned the pump off, and the speed jumped back to 2000. This was not an overwrite to a default. It was a collision: several commands, each carrying the speed, were in flight at once, and because the cache was only partially optimistic, an older cached speed was folded into the fused on/off command and overwrote a value that had already reached the pump.

After studying the component logs, working out what was going on, and reading your timing advice in #63, I concluded there is a structural issue here that deserves design discussion at a higher level than any one overwrite or timing value. In a nutshell, there has to be a better way. Let's define the problem.

## What a programmatic client has to do, and where it breaks

There are really two things any program driving the pump needs to do, and the current entity interface makes both of them hard.

**1. Write a sequence of related values without corrupting them.** A typical operation is: set the mode, set that mode's setpoint, and set the on/off state. Because the pump fuses mode, setpoint, and on/off into a single Class 10 command, any single-entity write has to reconstruct the other fields from the component's cache. If that cache is still in flight for either the mode or the setpoint when the next write lands, the reconstruction can send a stale value, and the pump ends up storing something the client never asked for. Turning the pump off right after a mode or setpoint change is the sharpest example: the off command carries the mode and setpoint with it, so it can clobber a value that had not finished settling.

**2. Read a value back to confirm it.** After writing a value, the client wants to know what actually happened: did the write stick, was it rejected (for example a temperature low above the high), or was it clamped (for example a 1500 RPM request clamped to 1650)? If it reads too soon it gets the optimistically cached value, which always agrees with what was requested and tells it nothing. There is no signal that says when the value has settled to the pump-confirmed result.

## This is not documented, and it is not guessable

The timing required to do either of these correctly is real and specific. From your own answers in #63:

- After changing a mode, a client must wait for the component's internal 5s readback plus the 5s entity poll before that mode's setpoints are trustworthy, so roughly 11 seconds.
- After writing a setpoint, it must wait for the 1.6s readback plus the 5s poll, so roughly 7 seconds.
- `ready_status` / `pump_ready` does not help here. It does not toggle during a mode change or a setpoint write, because `is_cache_valid()` intentionally does not track the per-mode setpoints, and `set_mode` does not invalidate the core cache.

None of this is documented anywhere outside that issue thread, and there is no way a client could derive it from the interface itself. To get the harness working at all, I inserted fixed multi-second sleeps between every operation: set a value, wait 7 seconds, set the next, wait again. That sort of works with very cautious delays built in between every command, but is this really how you're supposed to program against the component? Is this something we want to document and advise people to do?

## Who this affects

The harness is one client, but it is not a special case. Anyone controlling the pump programmatically hits this. My own recirc automation already reads back `pump_enabled` and other status to confirm the pump is doing what it was told. And a plain Home Assistant user writing a YAML automation that changes a mode and a setpoint is directly exposed, with no realistic chance of knowing any of the above. Someone who only ever toggles the pump on and off and reads a few sensors is fine today. The moment they touch modes or setpoints from an automation, they run into the same timing and clobbering problems, with none of the context needed to avoid them.

# Why this is structural, not a set of bugs

As I said, these overwrites were not one-offs; I hit them repeatedly while building the harness. I can work around each one by spacing operations out with long waits, but that is exactly the burden I am describing: to drive the pump programmatically today, a client has to understand the internal command structure and time around it, and even then it is fragile.

We also already know how much trouble the fused writes cause, because the record shows it. A number of the control issues you have fixed are the same problem surfacing in different places: the enable path sending a hardcoded setpoint instead of the stored one, a setpoint write turning the pump on and desyncing the on/off state, a mode change starting the pump, the pre-sync window overwriting a setpoint with the default. Each of those was a correct fix. The reason they keep appearing is that they are not really separate bugs. They are one architectural issue, the fused command reconstructed from an uncertain cache, showing up wherever a client touches more than one thing at a time.

The cost of leaving this at the point-fix level is not just the test harness. It is that programmatic control looks unreliable. A client that tries to set a mode and a setpoint and confirm them, and instead sees values silently change, will reasonably conclude that driving the pump from Home Assistant does not really work, and give up. That is a poor outcome for an interface that is otherwise very capable.

And the underlying delays are not something a fix can remove. A command travels from Home Assistant to the ESPHome component over the network, then from the component to the pump over BLE, and some settings have deliberate delays built into them. Nothing is instantaneous, and the entity interface gives the client no signal for what actually happened or when it finished happening. So a client is left guessing at delays it cannot see and cannot derive.

## What a programmatic interface should provide

Rather than chase individual timing values, it helps to state what a client actually needs from a programmatic interface to the pump. I would put it as six goals:

1. A client can write values without corrupting other values. Setting one thing, or a sequence of things, never changes a value the client did not touch, and the client does not need to know how the pump packs commands internally to avoid this.
2. Values the pump requires together can be set together, in one operation. Where mode and setpoint, or the two temperature limits and autoadapt, must reach the pump as a unit, the client can express them as a unit rather than as separate writes that race each other.
3. A client can tell when a write is fully settled, without guessing. There is a definite signal that a write has reached its final, pump-confirmed state, so the client never inserts fixed delays or depends on internal timing to know when it is safe to proceed or to read.
4. A client is told what actually happened to a write. Whether the value took as requested, was clamped to a different value, or was rejected, and ideally why. A write that failed or was rejected never appears to have succeeded.
5. Correct use is simple. A client can drive the pump correctly from the public, documented behavior alone, without reverse-engineering internal caches, readback delays, or poll intervals, and without a long list of precautions to follow. The straightforward way to use the interface should be the correct way.
6. Ideally, the above still hold when more than one client writes at once. The common case is a single writer, but the interface should not silently corrupt state or cross wires if two automations act at the same time.

## The realization

Measured against those goals, the current interface is not close, and not because of bugs. The individual entities meet essentially none of the write goals: a single-value entity write cannot express goal 2 at all, and it has no way to satisfy goal 1 without the client doing the sequencing and timing by hand. And there is no mechanism anywhere for goals 3 and 4. The entities publish a value on a fixed poll, with no indication of whether that value is the optimistic request or the settled result, and no channel to report a clamp or a rejection.

That is the core of it. The Home Assistant entity is the wrong tool for this job. It is a good fit for a single user-facing value that a person adjusts and watches, which is exactly what it should keep doing. It is a poor fit for a fused write that must carry several fields at once, and it has no ability to tell a client what happened and when it fully settled. No amount of delay tuning changes that, because the gap is in what the entity can express and report, not in how long you wait.

# A proposed direction

## The shape of the solution

The fix follows from the realization: use the right tool for each job. Keep the entities for what they are good at, reading state and letting a person adjust a value on a dashboard, and then add a small programmatic surface alongside them for clients that need to write and confirm. Three parts:

- Reading state stays on the entities. All the status and value sensors remain exactly as they are.
- Writing goes through service calls, one small set covering the whole write interface.
- Results come back on an event that reports what actually happened and when it settled.

The reason this is practical rather than a rewrite is that the write interface is small. Outside of schedules, everything a client can write is: on/off, the mode with its setpoint, the temperature range with autoadapt, and the cycle on and off times. That is a handful of operations, and each already has a method behind it in the component.

One scope note: this leaves out the pump scheduling interface, only because I have not worked with that side and cannot speak to it. It looks like it would fit the same pattern, service calls for its writes and events for its results, but I will leave that to you.

## Take on/off out of the fused path

Turning the pump on and off is by far the most common thing a client does, and it is the one operation that does not need to be fused with anything. This is an opportunity to keep the most common case as simple as it can be. The Class 3 START and STOP commands turn the pump on and off at its stored setpoint, carrying no mode or setpoint with them, and we demonstrated them working on the pump earlier. Giving on/off its own service backed by Class 3 lets a developer turn the pump on and off in the simplest possible way, with no mode or setpoint involved and the least chance of anything going wrong, while the individual pump settings stay exactly as they were last set.

This also removes a source of trouble on the UI side. Today the pump-enabled control is a fused Class 10 write, so even the dashboard on/off element can take part in a timing-related overwrite. Moving on/off to Class 3 takes that risk off the table for the entity as well as for programmatic callers.

Class 3 START and STOP were tracked separately after #43 and not adopted at the time. In this framing they have a clear purpose: they are how on/off leaves the fused path when that's all you're trying to do.

## Service calls for the write interface

The guiding rule is simple: each service call carries exactly the fields the pump fuses into a single write, no more and no less. Where the pump packs several values into one write, the service takes them together. Where it uses separate writes, the services stay separate.

There are a few of these fused writes. The Class 10 control command fuses the mode, the setpoint for that mode, and the on/off state, so the service that changes mode takes all three together, and the component sends exactly that with nothing reconstructed from a cache. The temperature range configuration is a separate fused write of its own, carrying the minimum, the maximum, and autoadapt together, so it becomes one service that takes those three. The cycle time configuration is another, carrying the on and off minutes together, so it becomes one service that takes those two. These configuration writes are their own objects, distinct from the mode and on/off control command, so their services carry only their own fields.

The individual entities do not go away. They remain for reading current state and for a person adjusting a value on a dashboard in the UI, which works fine at UI speed. Programmatic writes should go through the services.

## Results come back on an event

The read-back problem is solved by having the component report the result of each write on an event, instead of the client polling an entity and guessing. When a write settles, the component fires an event carrying what was written, the value the pump actually holds now, and whether it was accepted, clamped, or rejected, with a reason where there is one. The event is self-identifying, so a client can match a result to the write that produced it. Because the component is the only party that knows when the pump readback has landed, it decides when to send the event. The client times nothing. It makes the write and, if it cares about the result, waits for the event. The event carries the exact same set of parameters the service function did. If the service function had three parameters, the event contains all three with their settled values and status.

This is the same mechanism you already proposed for the backup and restore work in #79, where the component reports results back over an event rather than via an entity. This issue is asking for that same pattern applied to ordinary writes. An event is also a much better fit than an entity for reporting a specific error, such as a rejected temperature range, which entities communicate poorly.

For this to be usable the contract has to be strict: every write produces exactly one terminal event, including on rejection or timeout, because a client may be waiting on that event before it does anything else. A write that is superseded by a later write still gets a terminal event, so a waiting client never hangs.

## A sketch of the interface

To make this concrete, and to show how small it is, here is one way the interface could look. The names, arguments, and exact grouping are yours to decide. This is only to illustrate the shape.

The whole programmatic write interface, as service calls:

```
set_pump_enabled(enabled, op_id=None)                    # on/off only, Class 3, nothing else touched
set_mode(mode, setpoint, enabled, op_id=None)            # mode, setpoint, and on/off state as one Class 10 command
set_temperature_range(min, max, autoadapt, op_id=None)   # its own config object, written on its own
set_cycle_times(on_minutes, off_minutes, op_id=None)     # its own config object, written on its own
```

The `op_id` is a short identifier the client chooses so it can recognize the result of its own call. It defaults to nothing: a client that does not need to verify simply omits it and ignores the event.

When a write settles, the component fires one event:

```
event: esphome.alpha_hwr_write_settled
data:
  op_id:    "restore-speed-1"     # the id the caller passed, if any
  command:  "set_mode"            # which service this was
  mode:     "constant_speed"      # settled value
  setpoint: 2500                  # settled value, or the clamped value
  enabled:  true                  # settled value
  status:   "accepted"            # accepted, clamped, rejected, timeout, or superseded
  detail:   ""                    # a short reason when relevant
```

A serial client, such as the harness, then does the simplest possible thing: make the call, wait for the matching event, look at the result, move on. In pyscript that is:

```python
op_id = "restore-speed-1"
esphome.alpha_hwr_set_mode(mode="constant_speed", setpoint=2500, enabled=True, op_id=op_id)

result = task.wait_until(
    event_trigger=["esphome.alpha_hwr_write_settled", f"op_id == '{op_id}'"],
    timeout=15,
)
if result["status"] == "clamped":
    log.warning(f"speed clamped to {result['setpoint']}")
```

There are no fixed delays in that code. The component decides when the write is settled and says so, and the client waits for it. This removes all timing coupling between the client and the component for reading back values or knowing when things are settled or if they were successful. The component controls it all and the client just reacts to the events that the component sends.

## How this meets the goals

Against the six goals above:

1. Writes do not corrupt other values, because each service builds its command from the arguments passed, not from a cache, and on/off no longer carries a mode or setpoint at all.
2. Fused values are set together, because each service takes exactly the fields the protocol fuses in one call.
3. A client knows when a write is settled, because the component sends the settle event when the readback lands, and the client waits for it rather than guessing.
4. A client is told what happened, because the event reports accepted, clamped, or rejected, with a reason.
5. Correct use is simple, because the client makes one service call and optionally waits for one event, with nothing to know about internal caches, readback delays, or poll intervals. The straightforward way is the correct way.
6. Concurrent writers are handled in the expected order of last write wins, with no opportunity for mixing fused parameters from different clients, and the self-identifying event lets each client match the results of its own commands, with a caller-supplied identifier available where a client wants to be certain about the results of their specific command.

## A bounded, additive change

None of this is new capability. The services wrap methods the component already has. The event uses the component's existing ability to fire events to Home Assistant, sent at the same point where the component already performs its post-write readback. And it can be added without breaking anything: the services and the event go in alongside the existing entities, which keep working. Retiring or thinning the writable entities is not part of this, and I am not proposing it, since the entities are also how the dashboard is built.

The part that takes care is the strict event contract, making sure every write path ends in exactly one event across all of its outcomes. That is bounded by the same fact that makes the whole change practical: the write surface is small, so there are only a handful of paths to get right, and the logic that decides which result a write reached is the kind that can be unit tested on the host, as the existing control tests already are.

# This is your call

There is no urgency here. I will say that I have paused work on the test harness while this direction settles. I have spent more time understanding and working around timing issues than building the tool, and I would rather build the harness against whatever interface comes out of this than keep layering workarounds on top of the current one.

I also realize this is new and unplanned work, and that it takes the programming interface further than you may have intended the Home Assistant interface to go. That is genuinely your call. My view is that it is the one remaining step to a clean and powerful interface, one that matches how the component already has to talk to the pump, and that can tell a client what happened and when it settled. How far to take it, and whether to take it at all, is yours to decide.