function [] = ReceiveREADY(sm, cmd)
  [lines] = FSMClient('readlines', sm.handle);
  [m,n] = size(lines);
  line = lines(1,1:n);
  if isempty(findstr('READY', line)),  
      if (isempty(findstr('ERROR', line))),
        error(sprintf('RTLinux FSM Server did not send READY during %s command.', cmd)); 
      else
        error(sprintf('RTLinux FSM Server sent ERROR for %s command\n%s\n', cmd, line));           
      end;
end
