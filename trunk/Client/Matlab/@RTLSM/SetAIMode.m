% [sm]         = SetAIMode(sm, mode)
%                Set the data acquisition mode to use for the AI 
%                subdevice.  Possible modes to use are:
%                'asynch' -- asynchronous acquisition -- this is faster 
%                            since it happens independent of the FSM, but 
%                            is less compatible with all boards.
%                'synch'  -- synchronous acquisition -- the default
%                            works reliably with all boards.
%
function [sm] = SetAIMode(sm, mode)
    ChkConn(sm);
    if (~strcmp(mode,'asynch') & ~strcmp(mode,'synch')),
        error('Second argument to SetAIMode must be one of ''asynch'' or ''synch''');
    end;
    DoSimpleCmd(sm, sprintf('SET AIMODE %s', mode));
    return;
end
