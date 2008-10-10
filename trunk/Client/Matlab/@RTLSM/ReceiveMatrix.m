function [mat] = ReceiveMatrix(sm, cmd)

  lines = FSMClient('readlines', sm.handle);
  if (isempty(lines)), error(sprintf('%s error, empty result! Connection down?', cmd)); end;
  [m, n] = size(lines);
  if (m ~= 1), error(sprintf('%s got unexpected response.', cmd)); end;
  if (~isempty(strfind(lines(1,1:n), 'ERROR'))), error(sprintf('%s got ERROR response', cmd)); end;
  line = lines(1,1:n);

  [matM, matN] = strread(line, 'MATRIX %d %d');
  if (isempty(matM) || isempty(matN)), error(sprintf('%s got bogus response %s when querying matrix size.', cmd, line)); end;
  FSMClient('sendstring', sm.handle, sprintf('%s\n', 'READY'));
  mat = FSMClient('readmatrix', sm.handle, matM, matN);
