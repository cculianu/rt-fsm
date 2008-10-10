% [sm] = EnqueueSimulatedInputEvents(sm, timestamp, events_matrix)
%                Adds simulated input events described in the m x 2 matrix
%                `events_matrix' to the simulated input events queue.  
%
%                The `timestamp' parameter is a time (in FSM time, in
%                seconds, preferably in the future) at which to initiate
%                the clock latch countdown.  The emulator time will be
%                frozen at time: timestamp + clock_latch_time.
%                *** NOTE *** This argument is ignored on the real FSM and
%                only used by the emulator with its clock-latching
%                mechanism. (See GetClockLatch.m and SetClockLatch.m).
% 
%                Format for each row of the matrix is 
%                [ event_id timestamp_seconds ], where:
%
%                event_id is a positive number corresponding to an input
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
%                also GetSimulatedInputEvents.m and
%                ClearSimulatedInputEvents.m
%           
function [sm] = EnqueueSimulatedInputEvents(sm, ts, events_matrix)

  [m,n] = size(events_matrix);
  if (~isempty(events_matrix) & (~isnumeric(events_matrix) | n ~= 2) ),
      error('Please pass an m x 2 events matrix of doubles');
  end;
  events_matrix = double(events_matrix); % make sure it's a double array
  sm = ChkConn(sm);
  % now, send the matrix
  [res] = FSMClient('sendstring', sm.handle, sprintf('ENQUEUE SIMULATED INPUT EVENTS %f %u %u\n', ts, m, n));
  ReceiveREADY(sm, 'ENQUEUE SIMULATED INPUT EVENTS'); 
  [res] = FSMClient('sendmatrix', sm.handle, events_matrix);
  ReceiveOK(sm, 'ENQUEUE SIMULATED INPUT EVENTS');  

