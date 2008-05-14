function [ok] = ChkVersion(sm)

    ok = 0;
    verstr = '';
    
    try
        verstr = DoQueryCmd(sm, 'VERSION');
    catch
    end;
    
    ver = sscanf(verstr, '%u');
    if (~isempty(ver)),
        ver = ver(1);
        if (ver >= sm.MIN_SERVER_VERSION), ok = 1; end;
    end;

    if (~ok),
        error(sprintf('The FSM server does not meet the minimum protocol version requirement of %u', sm.MIN_SERVER_VERSION));
    end;

    % Now, tell the server about our version!
    DoSimpleCmd(sm, sprintf('CLIENTVERSION %u', sm.CLIENT_VERSION));

    
return;

