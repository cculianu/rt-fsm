function [sm] = ChkVersion(sm)

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
    toks = regexp(verstr, '\[\s*([^\]]+)\s*\]', 'tokens');
    tags = [];
    if (~isempty(toks)), 
        a = toks{1}; 
        a=regexp(a, '[0-9.]+\s+(\S+)$', 'tokens');
        if (~isempty(a)),
            tags = a{1}{1}{1};
        end;
    end;
    while (~isempty(tags) & length(tags)),
        [tag,tags] = strtok(tags, '.');
        if (~isempty(tag) & strcmp(tag, 'Emul')),
            sm.is_emul = 1;
        end;
    end;
      
    if (~ok),
        error(sprintf('The FSM server does not meet the minimum protocol version requirement of %u', sm.MIN_SERVER_VERSION));
    end;

    % Now, tell the server about our version!
    DoSimpleCmd(sm, sprintf('CLIENTVERSION %u', sm.CLIENT_VERSION));
   
return;

