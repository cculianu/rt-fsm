% [int nevents] = GetStimuliCounter(sm)   
%                Get the number of stimuli events that have occurred since
%                the last call to Initialize() (see also GetStimuli.m).
function [nevents] = GetStimuliCounter(sm)

  nevents = str2num(DoQueryCmd(sm, 'GET STIMULI COUNTER'));
  return;
 