function [result_matrix] = DoQueryMatrixCmd(varargin)

  if (nargin < 2), error('Need at least 2 argiments to DoQueryMatrixCmd'); end;
  sm = varargin{1};
  cmd = varargin{2};
  dorecvok = 1;
  if (nargin > 2), dorecvok = varargin{3}; end;
  
  ChkConn(sm);
  res = FSMClient('sendstring', sm.handle, sprintf('%s\n', cmd));
  if (isempty(res)), error(sprintf('%s error, connection down?', cmd)); end;
  mat = ReceiveMatrix(sm, cmd);
  if(dorecvok), 
  % grab the 'OK' at the end
   ReceiveOK(sm); 
  end;
  %fsmclient('disconnect');
  result_matrix = mat;
  % just to clean up the connection
  return;
end
