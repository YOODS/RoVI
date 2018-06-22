#!/usr/bin/env node

const NSycamctrl='/rovi/ycam_ctrl';
const NSpsgenpc='/rovi/pshift_genpc';
const NScamL='/rovi/left';
const NScamR='/rovi/right';
const NSlive='/rovi/live';
const NSrovi='/rovi';
const ros=require('rosnodejs');
const sensor_msgs=ros.require('sensor_msgs').msg;
const sensName=process.argv.length<3? 'ycam1s':process.argv[2];
const sens=require('./'+sensName+'.js')
const std_msgs=ros.require('std_msgs').msg;
const std_srvs=ros.require('std_srvs').srv;
const rovi_srvs = ros.require('rovi').srv;
const EventEmitter=require('events').EventEmitter;

const imgdbg = true;

// TODO from param_V live: camera: AcquisitionFrameRate: ?
const waitmsec_for_livestop = 300;

ros.Time.diff=function(t0){
	let t1=ros.Time.now();
	t1.secs-=t0.secs;
	t1.nsecs-=t0.nsecs;
	return ros.Time.toSeconds(t1);
}

class ImageSwitcher{
	constructor(node,ns){
		const who=this;
		this.node=node;
		this.raw=node.advertise(ns+'/image_raw', sensor_msgs.Image);
		this.rect=node.advertise(ns+'/image_rect', sensor_msgs.Image);
		this.vue=node.advertise(ns+'/view', sensor_msgs.Image);
		this.info=node.advertise(ns+'/camera_info', sensor_msgs.CameraInfo);
		this.remap=node.serviceClient(ns+'/remap',rovi_srvs.ImageFilter,{persist:true});
		setImmediate(async function(){
			if(! await node.waitForService(who.remap.getService(),2000)){
				ros.log.error('remap service not available');
				return;
			}
			try{
				who.param=await node.getParam(ns+'/camera');
				who.caminfo=Object.assign(new sensor_msgs.CameraInfo(),await node.getParam(ns+'/remap'));
			}
			catch(err){
				ros.log.warn('getParam '+err);
			}
		});
		this.hook=new EventEmitter();
		this.capt=[];
	}
	async emit(img){
		this.raw.publish(img);
		let req=new rovi_srvs.ImageFilter.Request();
		req.img=img;
		let res=await this.remap.call(req);
		if(this.hook.listenerCount('store')>0) this.hook.emit('store',res.img);
		else this.rect.publish(res.img);
		this.caminfo.header=req.img.header;
		this.caminfo.distortion_model="plumb_bob";
		this.info.publish(this.caminfo);
	}
	store(count){
		const who=this;
		this.capt=[];
		return new Promise(function(resolve){
			who.hook.on('store',function(img){
				if(who.capt.length<count){
					who.capt.push(img);
					if(who.capt.length==count){
						who.hook.removeAllListeners();
						resolve(who.capt);
					}
				}
			});
		});
	}
	cancel(){
		this.hook.removeAllListeners();
	}
	get ID(){ return this.param.ID;}
	view(n){
		if(n<this.capt.length) {
			this.vue.publish(this.capt[n]);
		}
	}
}

setImmediate(async function(){
	const rosNode=await ros.initNode(NSycamctrl);
	const image_L=new ImageSwitcher(rosNode,NScamL);
	const image_R=new ImageSwitcher(rosNode,NScamR);
	const pub_stat=rosNode.advertise(NSycamctrl+'/stat', std_msgs.Bool);
	let vue_N=0;
	const pub_pc=rosNode.advertise(NSrovi+'/pc', sensor_msgs.PointCloud);
	const pub_pc2=rosNode.advertise(NSrovi+'/pc2', sensor_msgs.PointCloud2);
	const genpc=rosNode.serviceClient(NSrovi+'/genpc',rovi_srvs.GenPC,{persist:true});
	if(! await rosNode.waitForService(genpc.getService(),2000)){
		ros.log.error('genpc service not available');
		return;
	}
	let param_P=await rosNode.getParam(NSpsgenpc+'/projector');
	let param_C=await rosNode.getParam(NSpsgenpc+'/camera');//-------camera param for phase shift mode
	let param_V=await rosNode.getParam(NSlive+'/camera');//-------camera param for streaming mode

	let sensEv;
	switch(sensName){
	case 'ycam1s':
		sensEv=sens.open(image_L.ID,image_R.ID,param_P.Url,param_P.Port,param_V);
		break;
	case 'ycam3':
		sensEv=sens.open(rosNode,NSrovi);
		break;
	}
	sensEv.on('stat',function(s){
		let f=new std_msgs.Bool();
		f.data=s;
		pub_stat.publish(f);
	});
	sensEv.on('wake',async function(s){
		param_V=await rosNode.getParam(NSlive+'/camera');
		sens.cset(param_V);
	});
	sensEv.on('left',async function(img){
		image_L.emit(img);
	});
	sensEv.on('right',async function(img){
		image_R.emit(img);
	});
//---------Definition of services
	let capt_L;//<--------captured images of the left camera
	let capt_R;//<--------same as right
	const svc_do=rosNode.advertiseService(NSpsgenpc,std_srvs.Trigger,(req,res)=>{//<--------generate PCL
if (imgdbg) {
ros.log.warn('service pshift_genpc called');
}
		if(!sens.normal){
			ros.log.warn(res.message='YCAM not ready');
			res.success=false;
			return true;
		}
		return new Promise(async (resolve)=>{
			param_P=await rosNode.getParam(NSpsgenpc+'/projector');


//			const timeoutmsec = param_P.Interval*20;
			// TODO tmp +10 and +280
			const timeoutmsec = (param_P.Interval+10)*20 + waitmsec_for_livestop + 280;
if (imgdbg) {
ros.log.warn('livestop and pshift_genpc setTimeout ' + timeoutmsec + ' msec');
}
			let wdt=setTimeout(function(){//<--------watch dog
				resolve(false);
				image_L.cancel();
				image_R.cancel();
				sens.cset(Object.assign({'TriggerMode':'Off'},param_V));
				ros.log.error('livestop and pshift_genpc timed out');
			}, timeoutmsec);
			sens.cset({'TriggerMode':'On'});
			param_C=await rosNode.getParam(NSpsgenpc+'/camera');
			sens.cset(param_C);
			param_V=await rosNode.getParam(NSlive+'/camera');
			for(let key in param_V) if(!param_C.hasOwnProperty(key)) delete param_V[key];

if (imgdbg) {
ros.log.warn('now await livestop and pshift_genpc');
}
await setTimeout(async function() {
if (imgdbg) {
ros.log.warn("after livestop, pshift_genpc function start");
}
			sens.pset('x'+param_P.ExposureTime);
			sens.pset((sensName.startsWith('ycam3')? 'o':'p')+param_P.Interval);
			let val=param_P.Intencity<256? param_P.Intencity:255;
			val=val.toString(16);
			sens.pset('i'+val+val+val);
			sens.pset(sensName.startsWith('ycam3')? 'o2':'p2');//<--------projector sequence start
if (imgdbg) {
ros.log.warn('after pset p2');
}
			let imgs=await Promise.all([ image_L.store(13), image_R.store(13) ]);
if (imgdbg) {
ros.log.warn('after await Promise.all (img_Ls and img_Rs resolve)');
}
//			ros.log.warn("before sensHook.removeAllListeners()");
			image_L.cancel();
			image_R.cancel();
//			ros.log.warn("after  sensHook.removeAllListeners()");
			clearTimeout(wdt);
			capt_L=imgs[0];
			capt_R=imgs[1];
if (imgdbg) {
ros.log.warn('capt_L and capt_R set. capt_L.length=' + capt_L.length + ", capt_R.length=" + capt_R.length);
}

if (imgdbg) {
			for (let li = 0; li < capt_L.length; li++) {
				ros.log.warn("Set capt_L[" + li + "].seq=" + capt_L[li].header.seq);
			}
			for (let ri = 0; ri < capt_R.length; ri++) {
				ros.log.warn("Set capt_R[" + ri + "].seq=" + capt_R[ri].header.seq);
			}

			ros.log.warn("genpc CALL");
}
			let gpreq = new rovi_srvs.GenPC.Request();
			gpreq.imgL = capt_L;
			gpreq.imgR = capt_R;
			let gpres=await genpc.call(gpreq);
			pub_pc.publish(gpres.pc);
			pub_pc2.publish(gpres.pc2);
if (imgdbg) {
			ros.log.warn('pc published');
			ros.log.warn("genpc DONE");
}

			sens.cset(Object.assign({'TriggerMode':'Off'},param_V));
			res.message='scan compelete:'+imgs[0].length;
			res.success=true;
			image_L.view(vue_N);
			image_R.view(vue_N);
			resolve(true);

if (imgdbg) {
ros.log.warn("pshift_genpc function end");
ros.log.warn('service pshift_genpc resolve true return');
}
			}, waitmsec_for_livestop); // これはcsetが実際に反映されるのを待つ時間も兼ねる(ライブの残りカスを捨てるのも)

		});
	});
	const svc_parse=rosNode.advertiseService(NSycamctrl+'/parse',rovi_srvs.Dialog,(req,res)=>{
		let cmd=req.hello;
		let lbk=cmd.indexOf('{');
		let obj={};
		if(lbk>0){
			cmd=req.hello.substring(0,lbk).trim();
			try {
				obj=JSON.parse(req.hello.substring(lbk));
			}
			catch(err){
				//ignore
			}
		}
		let cmds=cmd.split(' ');
		if(cmds.length>1) cmd=cmds.shift();
		switch(cmd){
		case 'cset':
			sens.cset(obj);
			for(let key in obj){
				rosNode.setParam(NSlive+'/camera/'+key,obj[key]);
			}
			return Promise.resolve(true);
		case 'pset':
			sens.pset(cmds[0]);
			return Promise.resolve(true);
		case 'stat'://<--------sensor(maybe YCAM) status query
			return new Promise((resolve)=>{
				res.answer=JSON.stringify(sens.stat());
				resolve(true);
			});
		case 'view':
			return new Promise((resolve)=>{
				try{
					vue_N=parseInt(cmds[0]);
				}
				catch(err){
					vue_N=0;
				}
				image_L.view(vue_N);
				image_R.view(vue_N);
				resolve(true);
			});
		}
	});
});