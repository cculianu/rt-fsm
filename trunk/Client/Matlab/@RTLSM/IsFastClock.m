% [boolean_flg] = IsFastClock(sm)
%
%                *****EMULATOR MODE ONLY COMMAND*****
%
%                Returns 1 if the FSM is currently running in 'fast clock'
%                mode, 0 otherwise.  
%
%                An FSM emulator running in 'fast clock' mode simulates
%                time as quickly as possible.  That is, without any sleeps
%                in the periodic loop of the FSM.  This is useful in
%                conjunction with the 'Simulated Input Events'
%                functionality.  See GetSimEvents.m and EnqueueSimEvents.m.
%
function [ret] = IsFastClock(sm)
    str = DoQueryCmd(sm, 'IS FAST CLOCK');
    ret = sscanf(str, '%d');
    return;
end
