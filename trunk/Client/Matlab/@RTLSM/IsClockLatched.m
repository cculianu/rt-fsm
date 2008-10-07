% [boolean_flg] = IsClockLatched(sm)
%
%                *****EMULATOR MODE ONLY COMMAND*****
%
%                Returns 1 if the FSM is currently paused due to the clock
%                latch.  If paused, enough time has passed between
%                subsequent calls to ClockLatchPing.m that the FSM has
%                stopped processing.  A call to ClockLatchPing.m,
%                Initialize.m, or SetClockLatch.m will unpause the FSM. 
%
%                A return value of 0 indicates that the FSM is not paused
%                due to clock latching. (Note that the FSM may still be
%                paused for another reason or may be invalid, etc).
%
%                Clock latching is a mechanism to limit the amount of
%                processing the state machine does by specifying a limit on
%                the emount of time that may elapse (in state machine time)
%                between external 'update' commands.  See
%                ClockLatchPing.m and SetLockLatch.m.
function [ret] = IsClockLatched(sm)
    ChkConn(sm);
    str = DoQueryCmd(sm, 'IS CLOCK LATCHED');
    ret = sscanf(str, '%d');
    return;
end
