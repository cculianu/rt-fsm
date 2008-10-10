% [sm]           = ClockLatchPing(sm)
%
%                *****EMULATOR MODE ONLY COMMAND*****
%
%                Ping the FSM's clock latch, and, if the FSM time is paused
%                due to a latched clock, unset the latch and resume 
%                execution of the FSM.
%
%                In other words: If clock latching is enabled it is 
%                necessary to call this function periodically.
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
function [sm] = ClockLatchPing(sm)
    ChkConn(sm);
    DoSimpleCmd(sm, 'CLOCK LATCH PING');
    return;
end
