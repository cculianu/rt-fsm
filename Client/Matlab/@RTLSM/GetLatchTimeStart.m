% [latchtime_secs] = GetLatchTimeStart(sm)
%
%                *****EMULATOR MODE ONLY COMMAND*****
%
%                Retrieve the time, in seconds, in emulator time, when the
%                clock latch countdown will start.  Emulator time will stop
%                increasing when: tNow >= tLatchStart + tLatchTimeout,
%                where tNow is the current emulator time, tLatchStart is
%                the return of this function, and tLatchTimeout is the
%                return value of GetLockLatch.m
%
%                This parameter is normally updated by the ts parameter
%                passed to EnqueueSimulatedInputEvents.m.
%
%                Clock latching is a mechanism to limit the amount of
%                processing the state machine does by specifying a limit on
%                the emount of time that may elapse (in state machine time)
%                between external 'update' commands.  See
%                ClockLatchPing.m and SetLockLatch.m.
function [ret] = GetLatchTimeStart(sm)
    if (~sm.is_emul),
        error('This command is only supported on the FSM emulator');
    end;
    str = DoQueryCmd(sm, 'GET LATCH TIME T0 SECS');
    ret = sscanf(str, '%f');
    return;
end
