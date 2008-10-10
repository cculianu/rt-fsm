% [simEvtList] = GetSimulatedInputEvents(sm)
%                Inspects the simulated input events queue for the current
%                FSM and retrieves them in a m x 2 matrix.  
% 
%                Format for each row of the matrix is 
%                [ event_id timestamp_seconds ], where:
%
%                event_id is an integer corresponding to an input
%                event number (as would be returned by GetEvents2.m or as
%                would be specified by SetInputEvents.m).  -1 indicates a
%                'timeout event'.
%
%                timestamp_seconds is the time at which the event should
%                occur in the future, in FSM time (the same time as would
%                be returned by GetTime.m).
%
%                Simulated input events are a mechanism by which the state
%                can be programmed to receive input events at specific
%                times in the future, for the purposes of simulating a real
%                experiment on a state machine program, for example.  See
%                also EnqueueSimulatedInputEvents.m and
%                ClearSimulatedInputEvents.m
%           
function [mat] = GetSimulatedInputEvents(sm)
    mat = DoQueryMatrixCmd(sm, 'GET SIMULATED INPUT EVENT QUEUE');

