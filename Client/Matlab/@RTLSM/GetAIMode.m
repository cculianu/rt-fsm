% [mode]       = GetAIMode(sm)
%                Retrieve the current data acquisition mode for the AI 
%                subdevice.  Possible modes returned are:
%                'asynch' -- asynchronous acquisition -- this is faster 
%                            since it happens independent of the FSM, but 
%                            is less compatible with all boards.
%                'synch'  -- synchronous acquisition -- the default
%                            works reliably with all boards.
%
function [ret] = GetAIMode(sm)
    ChkConn(sm);
    ret = DoQueryCmd(sm, 'GET AIMODE');
    return;
end
