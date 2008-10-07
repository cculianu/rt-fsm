% [sm]           = SetClockLatch(sm, ms)
%
%                *****EMULATOR MODE ONLY COMMAND*****
%
%                Set the clock latch  time to ms milliseconds for the state
%                machine.  A value of 0 indicates that clock latching is to
%                be disabled.
%
%                Note this command takes effect immediately (it does not
%                require a SetStateMatrix/SetStateProgram call to take
%                effect).
%
%                Clock latching is a mechanism to limit the amount of
%                processing the state machine does by specifying a limit on
%                the emount of time that may elapse (in state machine time)
%                between external 'update' commands.  See
%                ClockLatchPing.m and SetLockLatch.m.
function [sm] = SetClockLatch(sm, ms)
    ChkConn(sm);
    DoSimpleCmd(sm, sprintf('SET CLOCK LATCH MS %d', ms));
    return;
end
