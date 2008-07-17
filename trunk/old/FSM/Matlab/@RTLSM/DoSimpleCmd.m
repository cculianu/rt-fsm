function [res] = DoSimpleCmd(sm, cmd)

     ChkConn(sm);
     res = FSMClient('sendstring', sm.handle, sprintf('%s\n', cmd));
     if (isempty(res)), error(sprintf('Empty result for simple command %s, connection down?', cmd)); end;
     ReceiveOK(sm, cmd);
     return;
end
