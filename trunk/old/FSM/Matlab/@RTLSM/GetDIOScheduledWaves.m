% sched_matrix = GetDIOScheduledWaves(sm)  % Get Digital I/O line schedwaves
%
% This function returns the current scheduled waves matrix for Digital
% I/O lines registered with a state machine. Note that only if
% SetStateMatrix.m has been called will the registered scheduled waves
% be actually sent to the RT Linux machine, so be careful and don't
% assume that the ScheduledWaves returned here are already running
% unless you know that SetStateMatrix has been called.
%
% PARAMETERS:
% -----------
%
%  sm      An RTLSM object
%
%
% RETURNS:
% --------
%
% sched_matrix    an M by 7 numeric matrix, where M is the number of
%                 registered DIO scheduled waves. For the format of
%                 this matrix, see SetScheduledWaves.m
%

function [sched_matrix] = GetDIOScheduledWaves(sm)

   sched_matrix = sm.sched_waves;
   if isempty(sched_matrix), sched_matrix = zeros(0, 7); end;
   
   return;
