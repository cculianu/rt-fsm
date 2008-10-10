% [sm] = ClearSimulatedInputEvents(sm)
%                Forcefully clears (empties) the simulated input events
%                queue for the current FSM.  
% 
%                Simulated input events are a mechanism by which the state
%                can be programmed to receive input events at specific
%                times in the future, for the purposes of simulating a real
%                experiment on a state machine program, for example.  See
%                also EnqueueSimulatedInputEvents.m and
%                SetSimulatedInputEvents.m
%           
function [sm] = ClearSimulatedInputEvents(sm)
    res = DoSimpleCmd(sm, 'CLEAR SIMULATED INPUT EVENTS');
