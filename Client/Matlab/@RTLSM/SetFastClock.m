% [sm]  = SetFastClock(sm, bool_flag)
%
%                *****EMULATOR MODE ONLY COMMAND*****
%
%                Set the fast clock flag to 1 or 0 (fast clock enabled or 
%                disabled).
%
%                An FSM emulator running in 'fast clock' mode simulates
%                time as quickly as possible.  That is, without any sleeps
%                in the periodic loop of the FSM.  This is useful in
%                conjunction with the 'Simulated Input Events'
%                functionality.  See GetSimEvents.m and EnqueueSimEvents.m.
%
function [sm] = SetFastClock(sm, flg)
    if (~sm.is_emul),
        error('This command is only supported on the FSM emulator');
    end;
    sm = ChkConn(sm);
    if (isempty(flg)), flg = 0; end;
    DoSimpleCmd(sm, sprintf('SET FAST CLOCK %d', flg));
    return;
end
