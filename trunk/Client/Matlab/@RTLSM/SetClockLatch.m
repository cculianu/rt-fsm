% [sm] = SetClockLatch(sm, latch_time_secs)
%
%                *****EMULATOR MODE ONLY COMMAND*****
%
%                Set the clock latch  time to latch_time_secs secs for the
%                state machine.  A value of 0 indicates that clock latching
%                is to be disabled.
%
%                Note this command takes effect immediately (it does not
%                require a SetStateMatrix/SetStateProgram call to take
%                effect).
%
%                Note also that the clock latch time is reset should the 
%                FSM be reset via a call to Initialize.m.
%
%                Clock latching is a mechanism to limit the amount of
%                processing the state machine does by specifying a limit on
%                the emount of time that may elapse (in state machine time)
%                between external 'update' commands.  See
%                ClockLatchPing.m and SetLockLatch.m.
function [sm] = SetClockLatch(sm, t)
    DoSimpleCmd(sm, sprintf('SET CLOCK LATCH MS %d', t*1e3));
    return;
end
