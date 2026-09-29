const express=require("express");
const http=require("http");
const WebSocket=require("ws");

const app=express();
const server=http.createServer(app);
const wss=new WebSocket.Server({server,path:"/ws"});
const PORT=process.env.PORT||10000;
const DEVICE_TOKEN=process.env.DEVICE_TOKEN||"change-me";
let latestData=null,deviceCount=0,dashboardCount=0;

app.get("/",(req,res)=>res.json({service:"vehicle-health-websocket-relay",websocket:"/ws",deviceCount,dashboardCount,hasData:latestData!==null}));
app.get("/health",(req,res)=>res.status(200).send("OK"));

function broadcast(data){
 for(const client of wss.clients){
  if(client.readyState===WebSocket.OPEN && client.role==="dashboard")client.send(data);
 }
}

wss.on("connection",(ws,req)=>{
 const url=new URL(req.url,`http://${req.headers.host}`);
 const role=url.searchParams.get("role");

 if(role==="device"){
  if(url.searchParams.get("token")!==DEVICE_TOKEN){ws.close(1008,"Invalid device token");return;}
  ws.role="device";deviceCount++;
  if(latestData)ws.send(JSON.stringify(latestData));
  ws.on("message",raw=>{
   try{
    const data=JSON.parse(raw.toString());
    if(data.type!=="sensor")return;
    data.serverTime=Date.now();latestData=data;broadcast(JSON.stringify(data));
   }catch(e){console.error("Invalid device message:",e.message);}
  });
  ws.on("close",()=>deviceCount--);
 }else if(role==="dashboard"){
  ws.role="dashboard";dashboardCount++;
  if(latestData)ws.send(JSON.stringify(latestData));
  ws.on("close",()=>dashboardCount--);
 }else{
  ws.close(1008,"role must be device or dashboard");
 }
});

const heartbeat=setInterval(()=>{
 wss.clients.forEach(ws=>{
  if(ws.isAlive===false)return ws.terminate();
  ws.isAlive=false;ws.ping();
 });
},30000);

wss.on("connection",ws=>{ws.isAlive=true;ws.on("pong",()=>ws.isAlive=true);});
server.listen(PORT,()=>console.log(`Listening on ${PORT}`));

process.on("SIGTERM",()=>{
 clearInterval(heartbeat);
 server.close(()=>process.exit(0));
});
