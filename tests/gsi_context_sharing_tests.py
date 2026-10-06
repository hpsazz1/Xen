"""Real local processes with synthetic HTTP payloads, never CS2 or real Runtime."""
import argparse, json, pathlib, socket, subprocess, tempfile, time, urllib.request, urllib.error, uuid

def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1',0)); return s.getsockname()[1]
def until(fn, timeout=5):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        try:
            value=fn()
            if value: return value
        except (OSError, ValueError): pass
        time.sleep(.05)
    raise AssertionError('condition timed out')
def run(exe, fixture):
    processes=[]
    with tempfile.TemporaryDirectory(prefix='gsi-sharing-') as root:
        root=pathlib.Path(root)
        def launch_receiver(p):
            child=subprocess.Popen([fixture,str(p)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,encoding='utf8')
            processes.append(child)
            assert child.stdout.readline().strip()=='ready'
            return child
        def query(child, cmd='snapshot'):
            child.stdin.write(cmd+'\n'); child.stdin.flush()
            return json.loads(child.stdout.readline())
        def launch_lineup(p, suffix):
            web=port()
            cfg=root/(suffix+'.ini')
            cfg.write_text(f'[gsi]\nenabled=true\nbind_address=127.0.0.1\nport={p}\nttl_ms=1500\n',encoding='utf8')
            child=subprocess.Popen([exe,'--synthetic','--port',str(web),'--gsi-config',str(cfg),'--data',str(root/suffix)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            processes.append(child)
            def full_state():
                with urllib.request.urlopen(f'http://127.0.0.1:{web}/api/state',timeout=2) as r: return json.load(r)
            def state(): return full_state()['context']
            def command(action, **kwargs):
                for _ in range(20):
                    current=full_state()
                    body=dict(epoch=current['epoch'],revision=current['revision'],request_id=str(uuid.uuid4()),action=action,**kwargs)
                    req=urllib.request.Request(f'http://127.0.0.1:{web}/api/command',data=json.dumps(body).encode(),headers={'Content-Type':'application/json'})
                    try:
                        with urllib.request.urlopen(req,timeout=2) as response: return json.load(response)
                    except urllib.error.HTTPError as error:
                        if error.code!=409: raise
                        time.sleep(.03)
                raise AssertionError('command revision kept changing')
            state.full=full_state; state.command=command
            until(state)
            return child,state
        def send(p,map='de_dust2',team='T',observed=False,stamp=None):
            data={'provider':{'appid':730,'steamid':'76561198000000000','timestamp':stamp or int(time.time())},
                  'map':{'name':map,'phase':'live'},'round':{'phase':'live'},
                  'player':{'steamid':'76561198000000001' if observed else '76561198000000000','activity':'playing','team':team,
                            'state':{'health':100},'weapons':{'weapon_0':{'name':'weapon_ak47','state':'active','ammo_clip':30,'ammo_clip_max':30,'ammo_reserve':90}}}}
            req=urllib.request.Request(f'http://127.0.0.1:{p}/gsi',data=json.dumps(data).encode(),headers={'Content-Type':'application/json'})
            with urllib.request.urlopen(req,timeout=2) as r: assert r.status==200
        def next_second():
            current=int(time.time()); until(lambda:int(time.time())>current,2)
        try:
            p1,p2=port(),port()
            runtime=launch_receiver(p1)
            helper,state=launch_lineup(p1,'first')
            assert query(runtime)['error']==''
            send(p1)
            until(lambda:state()['auto_map']=='de_dust2')
            assert query(runtime)['valid'] and query(runtime)['weapon']=='ak47'
            # Helper has no GSI socket; second exclusive bind is still refused.
            with socket.socket() as s:
                try: s.bind(('127.0.0.1',p1))
                except OSError: pass
                else: raise AssertionError('exclusive listener lost')
            runtime2=launch_receiver(p2)
            helper2,state2=launch_lineup(p2,'second')
            send(p2,'de_mirage','CT')
            until(lambda:state2()['auto_map']=='de_mirage')
            assert state()['auto_map']!='de_mirage'
            # No incoming renewal: pipe heartbeat must not extend original GSI TTL.
            until(lambda:state()['auto_team']=='UNKNOWN',3)
            next_second(); send(p1,'de_inferno','CT')
            until(lambda:state()['auto_map']=='de_inferno' and state()['auto_team']=='CT')
            next_second(); send(p1,'de_inferno','CT',True)
            until(lambda:state()['auto_team']=='UNKNOWN')
            next_second(); send(p1,'de_nuke','T')
            until(lambda:state()['auto_map']=='de_nuke')
            # An old packet cannot resurrect older context.
            send(p1,'de_dust2','CT',stamp=int(time.time())-2)
            until(lambda:state()['auto_team']=='UNKNOWN')
            next_second(); send(p1,'de_anubis','CT')
            until(lambda:state()['auto_map']=='de_anubis')
            helper.terminate(); helper.wait(timeout=5)
            next_second(); send(p1,'de_dust2','T')
            assert query(runtime)['valid']
            helper,state=launch_lineup(p1,'restart-helper')
            next_second(); send(p1,'de_dust2','T')
            until(lambda:state()['auto_map']=='de_dust2')
            query(runtime,'stop'); until(lambda:state()['auto_team']=='UNKNOWN')
            query(runtime,'start'); next_second(); send(p1,'de_mirage','CT')
            until(lambda:state()['auto_map']=='de_mirage')
            runtime.terminate(); runtime.wait(timeout=5)
            until(lambda:state()['auto_team']=='UNKNOWN')
            runtime=launch_receiver(p1); next_second(); send(p1,'de_nuke','T')
            until(lambda:state()['auto_map']=='de_nuke')
            # 手动筛选固定条件，使 GSI TTL/重启不影响此次定位事件独立契约。
            state.command('context',mode='manual',map='de_nuke',team='T')
            state.command('start'); state.command('mode',value='capture')
            state.command('capture',map='de_nuke',region='synthetic',standpoint_id='',stance='stand',instructions='synthetic fixture')
            until(lambda:len(state.full()['recipes'])==1 and not state.full()['busy'])
            recipe=state.full()['recipes'][0]['id']
            state.command('update',items=[dict(id=recipe,name='locate fixture',target='synthetic',grenade='smoke',notes='',validation='unverified')])
            until(lambda:not state.full()['busy'] and not state.full()['recipes'][0]['draft'])
            state.command('mode',value='browse'); state.command('lock',id=recipe)
            assert not state.full()['locating']
            query(runtime,'locate'); until(lambda:state.full()['locating'])
            state.command('cancel'); state.command('lock',id=recipe)
            time.sleep(.2); assert not state.full()['locating'], 'heartbeat replayed locate'
            query(runtime,'locate-stale'); time.sleep(.2)
            assert not state.full()['locating'], 'expired edge executed'
            query(runtime,'stop'); until(lambda:state()['auto_team']=='UNKNOWN')
            query(runtime,'locate'); query(runtime,'start')
            time.sleep(.2)
            assert not state.full()['locating'], 'reconnect replayed pre-connection edge'
            query(runtime,'locate'); until(lambda:state.full()['locating'])
            # No publisher (old Runtime, wrong config or exited service) stays manual-safe.
            orphan,orphan_state=launch_lineup(port(),'unavailable')
            assert orphan_state()['auto_team']=='UNKNOWN'
            print('PASS: concurrent sources, Runtime control unchanged, TTL, observer, ordering, helper exit, receiver/process restart, missing publisher, locate once/TTL/reconnect')
        finally:
            for p in processes:
                if p.poll() is None: p.terminate()
            for p in processes: p.wait(timeout=5)
if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('--exe',required=True); p.add_argument('--fixture',required=True)
    a=p.parse_args(); run(a.exe,a.fixture)
