% [latchtime_ms] = GetClockLatch(sm)
%
%                *****EMULATOR MODE ONLY COMMAND*****
%
%                Retrieve the current clock latch setting, in MS, for the
%                state machine.  A value of 0 indicates that clock latching
%                is disabled.
%
%                Clock latching is a mechanism to limit the amount of
%                processing the state machine does by specifying a limit on
%                the emount of time that may elapse (in state machine time)
%                between external 'update' commands.  See
%                ClockLatchPing.m and SetLockLatch.m.
function [ret] = GetClockLatch(sm)
    ChkConn(sm);
    str = DoQueryCmd(sm, 'GET CLOCK LATCH MS');
    ret = sscanf(str, '%d');
    return;
end
