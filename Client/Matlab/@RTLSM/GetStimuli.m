% [stimmatrix] = GetStimuli(sm, startEventNumber, endEventNumber)
%                Retrieve information on what input events and what outputs
%                were deliverd by the FSM in the past.  Similar in spirit
%                to the GetEvents2.m function, but also reports on what
%                outputs occurred.
%
%                Parameter startEventNumber specifies a 1-indexed event
%                number to start from, and the endEventNumber is the last
%                event number to retrieve, inclusive.
%
%                The returned matrix is an mx4 matrix which encapsulates
%                all the input events and output the FSM did.  Each row
%                corresponds to an input or output event and the columns
%                have the following meaning:
%
%                1st - type
%                      a number indicating the type of the event. Possible
%                      values include:
%                             0 for INPUT 
%                             1 for Digital OUTPUT (DOUT)
%                             2 for Trigger OUTPUT
%                             3 for External (Sound) OUTPUT
%                             4 for Scheduled Wave OUTPUT
%                             5 for TCP OUTPUT
%                             6 for UDP OUTPUT
%                           127 for NOOP OUTPUT
%
%               2nd - id
%                    a number whose meaning depends on 'type' above.
%                    INPUT - for input events corresponds to the state
%                            matrix column number of the input event, this
%                            id will be the same id you would get from the
%                            GetEvents2.m 'event_id'.
%                    TIMEOUT - for timeouts will be a -1 (just like
%                              GetEvents2.m's event_id)
%                    OUTPUT  - for all others will correspond to the state 
%                              machine OUTPUT ROUTING output column number.
%                
%               3rd - value
%                    a number whose meaning depends on the 'type' above.
%                    INPUT/TIMEOUT - the state that was jumped to as a
%                                    result of this event ID.
%                    OUTPUT - the value of the output (eg, the DOUT lines
%                    set or the sound triggered, etc).
%
%               4th - timestamp
%                    the time at which the event occurred, in seconds (in
%                    FSM time, same time as one would get from GetTime.m).
%
%               See also: GetStimuliCounter.m and GetEvents2.m
function [mat] = GetStimuli(sm, start_no, end_no)
    if start_no > end_no,
        mat = zeros(0,4);
    else
        mat = DoQueryMatrixCmd(sm, sprintf('GET STIMULI %d %d', start_no-1, end_no-1));
    end;
    
    