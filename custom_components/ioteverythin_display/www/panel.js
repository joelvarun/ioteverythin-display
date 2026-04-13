/**
 * IoT Everythin Display - HA Custom Panel v1.3
 * Categories, Dimmable, RGB, Display Settings (brightness/timeout/battery)
 */
const GOLD='#D4A017',GOLD_B='#FFD700',DARK='#181818',BG='#111',BRAND='Touch-i';
const LIGHT_ICONS=['bulb','tube','fan','socket','alarm','warm'];
const CATEGORIES=['Hall','Dining','Balcony','Entrance','Toilet','Bedroom','Kitchen'];

class IotEverythinDisplayPanel extends HTMLElement {
  constructor(){
    super();
    this._hass=null;
    this._config={lights:[],climate:{temp_sensor:'',hum_sensor:'',acs:[]},sensors:{doors:[],motion:[]},display:{brightness:80,timeout:30,battery_timeout:10}};
    this._allEntities=[];
    this._activeTab='lights';
    this._deviceInfo=null;
    this._status='';
    this._loading=true;
  }

  set hass(hass){
    this._hass=hass;
    this._allEntities=Object.keys(hass.states).sort();
    if(!this._initialized){
      this._initialized=true;
      this._loadAll();
    } else if(!this.innerHTML){
      // Re-render if HA cleared our content (navigation away and back)
      this._loading=true;
      this._loadAll();
    }
  }

  async _loadAll(){
    console.log('[IoTDisplay] Panel init - loading device info and config...');
    await Promise.all([this._loadDeviceInfo(),this._loadCurrentConfig()]);
    // If device config is empty, try HA storage backup
    const hasConfig=this._config.lights.length>0||this._config.climate.acs?.length>0||this._config.sensors.doors?.length>0||this._config.sensors.motion?.length>0;
    if(!hasConfig){
      console.log('[IoTDisplay] Device config empty, trying HA storage...');
      await this._loadFromStorage();
    }
    this._loading=false;
    this._render();
  }

  async _loadFromStorage(){
    try{
      const r=await this._hass.callWS({type:'ioteverythin_display/load_panel_config'});
      if(r&&r.lights&&r.lights.length>0){
        console.log('[IoTDisplay] Restored config from HA storage:',r.lights?.length,'lights');
        if(r.lights) this._config.lights=r.lights;
        if(r.climate) this._config.climate={...this._config.climate,...r.climate};
        if(r.sensors){
          if(r.sensors.doors) this._config.sensors.doors=r.sensors.doors;
          if(r.sensors.motion) this._config.sensors.motion=r.sensors.motion;
        }
        if(r.display) this._config.display={...this._config.display,...r.display};
        this._status='Config restored from HA backup';
        setTimeout(()=>{this._status='';this._render();},5000);
      }
    }catch(e){
      console.warn('[IoTDisplay] load_panel_config:',e);
    }
  }

  async _saveToStorage(){
    try{
      const cfg={lights:this._config.lights,climate:this._config.climate,sensors:this._config.sensors,display:this._config.display};
      await this._hass.callWS({type:'ioteverythin_display/save_panel_config',config:cfg});
      console.log('[IoTDisplay] Config saved to HA storage');
    }catch(e){
      console.warn('[IoTDisplay] save_panel_config:',e);
    }
  }

  async _loadDeviceInfo(){
    try{
      const r=await this._hass.callWS({type:'ioteverythin_display/get_info'});
      console.log('[IoTDisplay] get_info OK:',JSON.stringify(r));
      this._deviceInfo=r;
    }catch(e){
      console.error('[IoTDisplay] get_info FAIL:',e);
      this._deviceInfo={error:String(e.message||e)};
    }
  }

  async _loadCurrentConfig(){
    try{
      const r=await this._hass.callWS({type:'ioteverythin_display/get_config'});
      console.log('[IoTDisplay] get_config OK - lights:',r.lights?.length,'acs:',r.climate?.acs?.length,
        'doors:',r.sensors?.doors?.length,'motion:',r.sensors?.motion?.length);
      if(r.lights&&r.lights.length>0) this._config.lights=r.lights;
      if(r.climate&&(r.climate.temp_sensor||r.climate.acs?.length))
        this._config.climate={...this._config.climate,...r.climate};
      if(r.sensors){
        if(r.sensors.doors) this._config.sensors.doors=r.sensors.doors;
        if(r.sensors.motion) this._config.sensors.motion=r.sensors.motion;
      }
      if(r.display){
        this._config.display={...this._config.display,...r.display};
      }
    }catch(e){
      console.error('[IoTDisplay] get_config FAIL:',e);
    }
  }

  async _pushConfig(){
    this._status='Pushing config to display...';
    this._render();
    try{
      const payload={ha_token:'',lights:this._config.lights,climate:this._config.climate,sensors:this._config.sensors,display:this._config.display};
      console.log('[IoTDisplay] Pushing config:',JSON.stringify(payload).length,'bytes');
      const r=await this._hass.callWS({type:'ioteverythin_display/push_config',config:payload});
      this._status='Config pushed! (v'+(r.version||'?')+')';
      console.log('[IoTDisplay] push OK:',r);
      // Auto-save to HA storage as backup
      await this._saveToStorage();
    }catch(e){
      const em=e.message||e.code||((typeof e==='object')?JSON.stringify(e):String(e));
      this._status='Error: '+em;
      console.error('[IoTDisplay] push FAIL:',e);
    }
    this._render();
    setTimeout(()=>{this._status='';this._render();},8000);
  }

  _friendly(eid){
    const s=this._hass?.states?.[eid];
    return s?.attributes?.friendly_name||eid.split('.').pop().replace(/_/g,' ');
  }

  _stateVal(eid){return this._hass?.states?.[eid]?.state||'?';}

  _getCats(){
    const custom=new Set();
    this._config.lights.forEach(l=>{if(l.cat&&!CATEGORIES.includes(l.cat))custom.add(l.cat);});
    return [...CATEGORIES,...custom];
  }

  _addLight(){
    const sel=this.querySelector('#light-picker');
    const catSel=this.querySelector('#light-cat');
    if(!sel?.value)return;
    const eid=sel.value;
    if(this._config.lights.find(l=>l.eid===eid))return;
    const domain=eid.split('.')[0];
    const attrs=this._hass.states[eid]?.attributes||{};
    const cm=attrs.supported_color_modes||[];
    let cat=catSel?.value||'Hall';
    if(cat==='__new__'){
      const n=prompt('Enter new area name:');
      if(!n)return;
      cat=n.trim().substring(0,18);
      if(!cat)return;
    }
    this._config.lights.push({
      eid,label:this._friendly(eid).substring(0,20),
      icon:domain==='light'?'bulb':'socket',
      dimmable:domain==='light'&&cm.some(m=>m!=='onoff'),
      rgb:!!cm.some(m=>['rgb','rgbw','rgbww','hs','xy'].includes(m)),
      cat,domain
    });
    this._render();
  }

  _addAC(){
    const sel=this.querySelector('#ac-picker');
    if(!sel?.value)return;
    if(this._config.climate.acs.find(a=>a.eid===sel.value))return;
    const attrs=this._hass.states[sel.value]?.attributes||{};
    this._config.climate.acs.push({eid:sel.value,label:this._friendly(sel.value).substring(0,20),min:attrs.min_temp||16,max:attrs.max_temp||30});
    this._render();
  }

  _addDoor(){
    const sel=this.querySelector('#door-picker');
    if(!sel?.value)return;
    if(this._config.sensors.doors.find(d=>d.eid===sel.value))return;
    this._config.sensors.doors.push({eid:sel.value,label:this._friendly(sel.value).substring(0,20),inverted:false});
    this._render();
  }

  _addMotion(){
    const sel=this.querySelector('#motion-picker');
    if(!sel?.value)return;
    if(this._config.sensors.motion.find(m=>m.eid===sel.value))return;
    this._config.sensors.motion.push({eid:sel.value,label:this._friendly(sel.value).substring(0,20)});
    this._render();
  }

  _render(){
    const info=this._deviceInfo;
    let infoHtml;
    if(this._loading){
      infoHtml='<div class="info-bar">Loading display info...</div>';
    }else if(!info){
      infoHtml='<div class="info-bar warn">Could not reach display</div>';
    }else if(info.error){
      infoHtml='<div class="info-bar warn">Display offline: '+info.error+'</div>';
    }else{
      infoHtml='<div class="info-bar ok"><b>'+(info.name||'Display')+'</b>'
        +'<span>IP: '+(info.ip||'?')+'</span>'
        +'<span>FW: '+(info.version||'?')+'</span>'
        +'<span>MAC: '+(info.mac||'?')+'</span>'
        +(info.battery_mv!==undefined?'<span>🔋 '+(info.battery_pct||0)+'% ('+info.battery_mv+'mV)'+(info.charging?' ⚡':'')+'</span>':'')
        +'<span>💡 '+(info.brightness||'?')+'%</span>'
        +'<span class="dot '+(info.configured?'green':'orange')+'"></span>'
        +'<span>'+(info.configured?'Configured':'Needs config')+'</span></div>';
    }

    const nL=this._config.lights.length;
    const nAC=this._config.climate.acs.length;
    const nD=this._config.sensors.doors?.length||0;
    const nM=this._config.sensors.motion?.length||0;

    this.innerHTML=`<style>
:host{display:block;padding:16px;font-family:Roboto,sans-serif;color:#eee;background:${BG};min-height:100vh}
.hdr{display:flex;align-items:center;gap:12px;margin-bottom:12px}
.hdr h1{color:${GOLD};margin:0;font-size:22px}
.info-bar{background:${DARK};padding:10px 16px;border-radius:10px;margin-bottom:12px;display:flex;gap:14px;align-items:center;font-size:13px;flex-wrap:wrap}
.info-bar.warn{border:1px solid #c0392b;color:#e74c3c}
.info-bar.ok{border:1px solid #333}
.dot{width:10px;height:10px;border-radius:50%;display:inline-block}
.dot.green{background:#2ecc71}.dot.orange{background:${GOLD}}
.tabs{display:flex;gap:0;margin-bottom:14px;border-bottom:1px solid #333}
.tab{padding:10px 20px;cursor:pointer;color:#777;font-size:15px;font-weight:500;border-bottom:2px solid transparent}
.tab.active{color:${GOLD};border-bottom-color:${GOLD}}
.tab:hover{color:${GOLD_B}}
.sec{background:${DARK};border-radius:10px;padding:14px;margin-bottom:10px}
.sec h3{color:${GOLD};margin:0 0 10px 0;font-size:15px}
.cat-group{margin-bottom:12px}
.cat-label{color:${GOLD};font-size:13px;font-weight:600;margin-bottom:6px;text-transform:uppercase;letter-spacing:.5px}
.erow{display:flex;align-items:center;gap:6px;padding:5px 8px;background:#222;border-radius:6px;margin-bottom:4px;font-size:13px;flex-wrap:wrap}
.erow .eid{color:#666;font-size:11px;flex:1;min-width:120px}
.erow .fname{color:#bbb;font-size:12px;min-width:100px;font-weight:500}
.erow .st{font-size:11px;padding:2px 6px;border-radius:4px;font-weight:600}
.erow .st.on{background:#1a3a1a;color:#2ecc71}
.erow .st.off{background:#2a1a1a;color:#666}
.erow input[type=text]{background:#333;color:#eee;border:1px solid #555;border-radius:4px;padding:3px 6px;width:110px;font-size:12px}
.erow select{background:#333;color:#eee;border:1px solid #555;border-radius:4px;padding:3px;font-size:12px}
.erow label{font-size:11px;color:#aaa;display:flex;align-items:center;gap:3px}
.rm{cursor:pointer;color:#c0392b;font-weight:bold;font-size:14px;padding:0 4px}
.add-row{display:flex;gap:6px;align-items:center;flex-wrap:wrap;margin-top:8px}
.add-row select{background:#222;color:#eee;border:1px solid #444;border-radius:6px;padding:7px;font-size:13px;max-width:320px;flex:1}
.add-row button,.push-bar button{background:${GOLD};color:#000;border:none;border-radius:6px;padding:8px 16px;cursor:pointer;font-weight:bold;font-size:13px}
.add-row button:hover,.push-bar button:hover{background:${GOLD_B}}
.push-bar{display:flex;gap:12px;align-items:center;margin-top:14px}
.push-bar button{padding:12px 28px;font-size:15px;border-radius:8px}
.push-bar .status{color:#aaa;font-size:13px}
.field-row{display:flex;gap:8px;align-items:center;margin-bottom:6px}
.field-row label{color:#aaa;font-size:13px;min-width:110px}
.field-row select,.field-row input{background:#222;color:#eee;border:1px solid #444;border-radius:6px;padding:6px;font-size:13px;flex:1;max-width:320px}
.refresh-btn{background:transparent;color:${GOLD};border:1px solid ${GOLD};border-radius:6px;padding:6px 12px;cursor:pointer;font-size:12px;margin-left:auto}
.refresh-btn:hover{background:${GOLD};color:#000}
</style>
<div class="hdr"><h1>Touch-i</h1><span style="color:#777;font-size:13px;margin-top:4px">by IoT Everythin</span><button class="refresh-btn" id="refresh-btn">Refresh</button></div>
${infoHtml}
<div class="tabs">
<div class="tab ${this._activeTab==='lights'?'active':''}" data-tab="lights">Lights (${nL})</div>
<div class="tab ${this._activeTab==='climate'?'active':''}" data-tab="climate">Climate (${nAC} ACs)</div>
<div class="tab ${this._activeTab==='sensors'?'active':''}" data-tab="sensors">Sensors (${nD+nM})</div>
<div class="tab ${this._activeTab==='display'?'active':''}" data-tab="display">Display</div>
</div>
<div id="tab-content">${this._renderTab()}</div>
<div class="push-bar"><button id="push-btn">Push Config to Display</button><span class="status">${this._status}</span></div>`;

    this.querySelectorAll('.tab').forEach(t=>t.addEventListener('click',()=>{this._activeTab=t.dataset.tab;this._render();}));
    this.querySelector('#push-btn')?.addEventListener('click',()=>this._pushConfig());
    this.querySelector('#refresh-btn')?.addEventListener('click',()=>{this._loading=true;this._render();this._loadAll();});
    this._bindTab();
  }

  _renderTab(){
    switch(this._activeTab){
      case'lights':return this._renderLights();
      case'climate':return this._renderClimate();
      case'sensors':return this._renderSensors();
      case'display':return this._renderDisplay();
    }return'';
  }

  _renderLights(){
    const lights=this._config.lights;
    const cats={};
    lights.forEach((l,i)=>{const c=l.cat||'Hall';if(!cats[c])cats[c]=[];cats[c].push({...l,_idx:i});});
    let groupsHtml='';
    for(const[cat,items]of Object.entries(cats)){
      let rows='';
      for(const l of items){
        const st=this._stateVal(l.eid);
        const stC=st==='on'?'on':'off';
        const allCats=this._getCats();
        rows+=`<div class="erow">
<span class="fname">${this._friendly(l.eid)}</span>
<input type="text" value="${(l.label||'').replace(/"/g,'&quot;')}" data-lbl="${l._idx}" placeholder="Alias" title="Display name on screen">
<span class="eid">${l.eid}</span>
<span class="st ${stC}">${st}</span>
<select data-icon="${l._idx}">${LIGHT_ICONS.map(ic=>'<option value="'+ic+'"'+(l.icon===ic?' selected':'')+'>'+ic+'</option>').join('')}</select>
<select data-cat="${l._idx}">${allCats.map(c=>'<option value="'+c+'"'+((l.cat||'Hall')===c?' selected':'')+'>'+c+'</option>').join('')}<option value="__new__">+ New Area...</option></select>
<label><input type="checkbox" data-dim="${l._idx}"${l.dimmable?' checked':''}> Dim</label>
<label><input type="checkbox" data-rgb="${l._idx}"${l.rgb?' checked':''}> RGB</label>
<span class="rm" data-rm-light="${l._idx}">&#10005;</span>
</div>`;
      }
      groupsHtml+=`<div class="cat-group"><div class="cat-label">${cat} (${items.length})</div>${rows}</div>`;
    }
    if(!groupsHtml) groupsHtml='<div style="color:#555;padding:8px">No light entities added yet</div>';
    const lightEnts=this._allEntities.filter(e=>e.startsWith('light.')||e.startsWith('switch.'));
    return `<div class="sec"><h3>Light &amp; Switch Entities</h3>${groupsHtml}
<div class="add-row">
<select id="light-picker"><option value="">Select light/switch entity...</option>${lightEnts.map(e=>'<option value="'+e+'">'+this._friendly(e)+' ('+e+')</option>').join('')}</select>
<select id="light-cat">${this._getCats().map(c=>'<option value="'+c+'">'+c+'</option>').join('')}<option value="__new__">+ New Area...</option></select>
<button id="add-light-btn">+ Add</button></div></div>`;
  }

  _renderClimate(){
    const allS=this._allEntities.filter(e=>e.startsWith('sensor.'));
    const tempS=allS.filter(e=>{const a=this._hass.states[e]?.attributes;return a?.device_class==='temperature'||e.includes('temp')||e.includes('therm');});
    const humS=allS.filter(e=>{const a=this._hass.states[e]?.attributes;return a?.device_class==='humidity'||e.includes('humid');});
    const climEnts=this._allEntities.filter(e=>e.startsWith('climate.'));
    const tVal=this._stateVal(this._config.climate.temp_sensor);
    const hVal=this._stateVal(this._config.climate.hum_sensor);
    const acRows=this._config.climate.acs.map((ac,i)=>{
      const st=this._stateVal(ac.eid);
      const ct=this._hass.states[ac.eid]?.attributes?.current_temperature||'?';
      const tt=this._hass.states[ac.eid]?.attributes?.temperature||'?';
      return `<div class="erow">
<span class="fname">${this._friendly(ac.eid)}</span>
<input type="text" value="${(ac.label||'').replace(/"/g,'&quot;')}" data-ac-lbl="${i}" placeholder="Alias" title="Display name on screen">
<span class="eid">${ac.eid}</span>
<span class="st ${st==='off'?'off':'on'}">${st} ${ct}deg->${tt}deg</span>
<label>Min:<input type="number" value="${ac.min}" data-ac-min="${i}" style="width:45px"></label>
<label>Max:<input type="number" value="${ac.max}" data-ac-max="${i}" style="width:45px"></label>
<span class="rm" data-rm-ac="${i}">&#10005;</span></div>`;
    }).join('');
    return `<div class="sec"><h3>Temperature &amp; Humidity</h3>
<div class="field-row"><label>Temp sensor ${tVal!=='?'?'('+tVal+'deg)':''}:</label>
<select id="temp-sensor-pick"><option value="">None</option>${tempS.map(e=>'<option value="'+e+'"'+(this._config.climate.temp_sensor===e?' selected':'')+'>'+this._friendly(e)+' ('+e+')</option>').join('')}</select></div>
<div class="field-row"><label>Humidity ${hVal!=='?'?'('+hVal+'%)':''}:</label>
<select id="hum-sensor-pick"><option value="">None</option>${humS.map(e=>'<option value="'+e+'"'+(this._config.climate.hum_sensor===e?' selected':'')+'>'+this._friendly(e)+' ('+e+')</option>').join('')}</select></div></div>
<div class="sec"><h3>Air Conditioners</h3>${acRows||'<div style="color:#555;padding:8px">No ACs added</div>'}
<div class="add-row"><select id="ac-picker"><option value="">Select climate entity...</option>${climEnts.map(e=>'<option value="'+e+'">'+this._friendly(e)+' ('+e+')</option>').join('')}</select>
<button id="add-ac-btn">+ Add</button></div></div>`;
  }

  _renderSensors(){
    const bs=this._allEntities.filter(e=>e.startsWith('binary_sensor.'));
    const doorEnts=bs.filter(e=>{const a=this._hass.states[e]?.attributes;return a?.device_class==='door'||a?.device_class==='window'||a?.device_class==='opening'||e.includes('door')||e.includes('contact');});
    const motionEnts=bs.filter(e=>{const a=this._hass.states[e]?.attributes;return a?.device_class==='motion'||a?.device_class==='occupancy'||a?.device_class==='presence'||e.includes('motion')||e.includes('occupancy');});
    const doorRows=(this._config.sensors.doors||[]).map((d,i)=>{
      const st=this._stateVal(d.eid);
      const isOpen=d.inverted?st==='off':st==='on';
      return `<div class="erow">
<span class="fname">${this._friendly(d.eid)}</span>
<input type="text" value="${(d.label||'').replace(/"/g,'&quot;')}" data-door-lbl="${i}" placeholder="Alias" title="Display name on screen">
<span class="eid">${d.eid}</span>
<span class="st ${isOpen?'on':'off'}">${isOpen?'OPEN':'closed'}</span>
<label><input type="checkbox" data-door-inv="${i}"${d.inverted?' checked':''}> Inverted</label>
<span class="rm" data-rm-door="${i}">&#10005;</span></div>`;
    }).join('');
    const motionRows=(this._config.sensors.motion||[]).map((m,i)=>{
      const st=this._stateVal(m.eid);
      return `<div class="erow">
<span class="fname">${this._friendly(m.eid)}</span>
<input type="text" value="${(m.label||'').replace(/"/g,'&quot;')}" data-motion-lbl="${i}" placeholder="Alias" title="Display name on screen">
<span class="eid">${m.eid}</span>
<span class="st ${st==='on'?'on':'off'}">${st==='on'?'ACTIVE':'clear'}</span>
<span class="rm" data-rm-motion="${i}">&#10005;</span></div>`;
    }).join('');
    return `<div class="sec"><h3>Door / Contact Sensors</h3>${doorRows||'<div style="color:#555;padding:8px">No door sensors added</div>'}
<div class="add-row"><select id="door-picker"><option value="">Select door/contact sensor...</option>${doorEnts.map(e=>'<option value="'+e+'">'+this._friendly(e)+' ('+e+')</option>').join('')}</select>
<button id="add-door-btn">+ Add</button></div></div>
<div class="sec"><h3>Motion / Occupancy Sensors</h3>${motionRows||'<div style="color:#555;padding:8px">No motion sensors added</div>'}
<div class="add-row"><select id="motion-picker"><option value="">Select motion/occupancy sensor...</option>${motionEnts.map(e=>'<option value="'+e+'">'+this._friendly(e)+' ('+e+')</option>').join('')}</select>
<button id="add-motion-btn">+ Add</button></div></div>`;
  }

  _renderDisplay(){
    const d=this._config.display||{};
    const br=d.brightness||80;
    const to=d.timeout||30;
    const bto=d.battery_timeout!=null?d.battery_timeout:10;
    const info=this._deviceInfo||{};
    const batHtml=info.battery_mv!==undefined
      ?`<div class="sec"><h3>Battery &amp; Power</h3>
<div class="field-row"><label>Battery:</label><span>${info.battery_pct||0}% (${info.battery_mv||0} mV)</span></div>
<div class="field-row"><label>Charging:</label><span>${info.charging?'Yes (USB connected)':'No (battery)'}</span></div>
<div class="field-row"><label>Backlight:</label><span>${info.backlight_on?'ON':'OFF (sleeping)'} — ${info.brightness||'?'}%</span></div>
</div>`:''
    const toOpts=[['0','Disabled (always on)'],['10','10 seconds'],['15','15 seconds'],['30','30 seconds'],['60','1 minute'],['120','2 minutes'],['180','3 minutes'],['300','5 minutes'],['600','10 minutes']];
    const mkOpts=(val)=>toOpts.map(([v,l])=>`<option value="${v}"${+val==+v?' selected':''}>${l}</option>`).join('');
    return `${batHtml}
<div class="sec"><h3>Screen Settings</h3>
<div class="field-row"><label>Brightness (${br}%):</label>
<input type="range" id="disp-bright" min="10" max="100" step="5" value="${br}" style="flex:1;max-width:320px;accent-color:${GOLD}">
</div>
<div class="field-row"><label>Timeout (USB):</label>
<select id="disp-timeout">${mkOpts(to)}</select></div>
<div class="field-row"><label>Timeout (Battery):</label>
<select id="disp-bat-timeout">${mkOpts(bto)}</select></div>
<div style="color:#777;font-size:12px;margin-top:8px">Separate timeouts for USB-powered and battery mode. Screen turns off after no touch. Touch to wake.</div>
</div>`;
  }

  _bindTab(){
    this.querySelector('#add-light-btn')?.addEventListener('click',()=>this._addLight());
    this.querySelectorAll('[data-rm-light]').forEach(el=>el.addEventListener('click',()=>{this._config.lights.splice(+el.dataset.rmLight,1);this._render();}));
    this.querySelectorAll('[data-lbl]').forEach(el=>el.addEventListener('change',()=>{this._config.lights[+el.dataset.lbl].label=el.value;}));
    this.querySelectorAll('[data-icon]').forEach(el=>el.addEventListener('change',()=>{this._config.lights[+el.dataset.icon].icon=el.value;}));
    this.querySelectorAll('[data-cat]').forEach(el=>el.addEventListener('change',()=>{
      let v=el.value;
      if(v==='__new__'){const n=prompt('Enter new area name:');if(!n){el.value=this._config.lights[+el.dataset.cat].cat||'Hall';return;}v=n.trim().substring(0,18);if(!v){el.value=this._config.lights[+el.dataset.cat].cat||'Hall';return;}}
      this._config.lights[+el.dataset.cat].cat=v;this._render();
    }));
    this.querySelectorAll('[data-dim]').forEach(el=>el.addEventListener('change',()=>{this._config.lights[+el.dataset.dim].dimmable=el.checked;}));
    this.querySelectorAll('[data-rgb]').forEach(el=>el.addEventListener('change',()=>{this._config.lights[+el.dataset.rgb].rgb=el.checked;}));
    this.querySelector('#temp-sensor-pick')?.addEventListener('change',e=>{this._config.climate.temp_sensor=e.target.value;});
    this.querySelector('#hum-sensor-pick')?.addEventListener('change',e=>{this._config.climate.hum_sensor=e.target.value;});
    this.querySelector('#add-ac-btn')?.addEventListener('click',()=>this._addAC());
    this.querySelectorAll('[data-rm-ac]').forEach(el=>el.addEventListener('click',()=>{this._config.climate.acs.splice(+el.dataset.rmAc,1);this._render();}));
    this.querySelectorAll('[data-ac-lbl]').forEach(el=>el.addEventListener('change',()=>{this._config.climate.acs[+el.dataset.acLbl].label=el.value;}));
    this.querySelectorAll('[data-ac-min]').forEach(el=>el.addEventListener('change',()=>{this._config.climate.acs[+el.dataset.acMin].min=+el.value;}));
    this.querySelectorAll('[data-ac-max]').forEach(el=>el.addEventListener('change',()=>{this._config.climate.acs[+el.dataset.acMax].max=+el.value;}));
    this.querySelector('#add-door-btn')?.addEventListener('click',()=>this._addDoor());
    this.querySelectorAll('[data-rm-door]').forEach(el=>el.addEventListener('click',()=>{this._config.sensors.doors.splice(+el.dataset.rmDoor,1);this._render();}));
    this.querySelectorAll('[data-door-lbl]').forEach(el=>el.addEventListener('change',()=>{this._config.sensors.doors[+el.dataset.doorLbl].label=el.value;}));
    this.querySelectorAll('[data-door-inv]').forEach(el=>el.addEventListener('change',()=>{this._config.sensors.doors[+el.dataset.doorInv].inverted=el.checked;}));
    this.querySelector('#add-motion-btn')?.addEventListener('click',()=>this._addMotion());
    this.querySelectorAll('[data-rm-motion]').forEach(el=>el.addEventListener('click',()=>{this._config.sensors.motion.splice(+el.dataset.rmMotion,1);this._render();}));
    this.querySelectorAll('[data-motion-lbl]').forEach(el=>el.addEventListener('change',()=>{this._config.sensors.motion[+el.dataset.motionLbl].label=el.value;}));
    // Display settings bindings
    this.querySelector('#disp-bright')?.addEventListener('input',e=>{
      const v=+e.target.value;this._config.display.brightness=v;
      const lbl=e.target.closest('.field-row')?.querySelector('label');
      if(lbl)lbl.textContent='Brightness ('+v+'%):';
    });
    this.querySelector('#disp-timeout')?.addEventListener('change',e=>{this._config.display.timeout=+e.target.value;});
    this.querySelector('#disp-bat-timeout')?.addEventListener('change',e=>{this._config.display.battery_timeout=+e.target.value;});
  }
}
customElements.define('ioteverythin-display-panel',IotEverythinDisplayPanel);
