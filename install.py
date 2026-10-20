#!/usr/bin/env python3
from pathlib import Path
import argparse,hashlib,json,re,shutil,sqlite3

root=Path(__file__).resolve().parent
parser=argparse.ArgumentParser(description='Install NVK Bootstrap into Minecraft instances without changing their selected rendering backend.')
parser.add_argument('--loader',choices=['fabric','forge','neoforge'])
parser.add_argument('--game-dir',type=Path)
parser.add_argument('--legacy-neoforge',action='store_true',help='Disable the early loading window for NeoForge using ModLauncher.')
args=parser.parse_args()
if bool(args.loader)!=bool(args.game_dir):parser.error('--loader and --game-dir must be used together')
base=Path.home()/'Library/Application Support/Swift Craft Launcher'
backup=root/'installation-backup';backup.mkdir(exist_ok=True)
if args.game_dir:
 targets=[(args.game_dir.resolve().name,args.game_dir.resolve(),args.loader)]
else:
 connection=sqlite3.connect('file:'+str(base/'data/data.db')+'?mode=ro',uri=True)
 targets=[]
 for name,raw in connection.execute('select game_name,data_json from game_versions'):
  data=json.loads(raw);loader=str(data.get('modLoader','')).lower()
  if loader in ['fabric','forge','neoforge']:targets.append((name,base/'profiles'/name,loader))
 connection.close()
records=[]
for name,game,loader in targets:
 jar=root/('dist/nvk-bootstrap-0.3.0-'+loader+'.jar')
 if not jar.is_file():raise FileNotFoundError(jar)
 if not game.is_dir():raise NotADirectoryError(game)
 digest=hashlib.sha256(jar.read_bytes()).hexdigest();mods=game/'mods';mods.mkdir(exist_ok=True)
 folder=backup/(re.sub(r'[^\w.\-]','_',name)+'-'+hashlib.sha256(str(game).encode()).hexdigest()[:8]);folder.mkdir(exist_ok=True)
 changes=[]
 for pattern in ['local-zink-bootstrap-*.jar','nvk-bootstrap-*.jar']:
  for old in mods.glob(pattern):
   if old.name==jar.name and hashlib.sha256(old.read_bytes()).hexdigest()==digest:continue
   destination=folder/(hashlib.sha256(old.read_bytes()).hexdigest()[:12]+'-'+old.name)
   if not destination.exists():shutil.copy2(old,destination)
   old.unlink();changes.append(str(destination))
 if loader=='forge' or (loader=='neoforge' and args.legacy_neoforge):
  config=game/'config/fml.toml';config.parent.mkdir(exist_ok=True)
  before=config.read_text() if config.exists() else ''
  after=re.sub(r'^earlyWindowControl\s*=.*$','earlyWindowControl = false',before,flags=re.M) if re.search(r'^earlyWindowControl\s*=',before,re.M) else 'earlyWindowControl = false\n'+before
  if after!=before:
   saved=folder/('fml-'+hashlib.sha256(before.encode()).hexdigest()[:12]+'.toml')
   if not saved.exists():saved.write_text(before)
   config.write_text(after);changes.append(str(saved))
 temp=mods/(jar.name+'.part');shutil.copy2(jar,temp);temp.replace(mods/jar.name)
 records.append({'instance':name,'game_directory':str(game),'loader':loader,'jar':str(mods/jar.name),'sha256':digest,'backups':changes})
p=root/'installation-0.3.0.json';p.write_text(json.dumps(records,ensure_ascii=False,indent=2));print(json.dumps(records,ensure_ascii=False,indent=2))
