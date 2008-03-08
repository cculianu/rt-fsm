function [] = ReceiveOK(sm, cmd)
  lines = FSMClient('readlines', sm.handle);
  [m,n] = size(lines);
  line = '';
  if (m & n),
      line = lines(1,1:n);
  end;
  if isempty(findstr('OK', line)),  
      errstr = sprintf('RTLinux FSM Server did not send OK after %s command.', cmd);
      if (m | n), errstr = sprintf('%s\nInstead it sent:\n\n >> %s', errstr, line); end;      
      error(sprintf('%s\n', errstr)); 
  end;
end
