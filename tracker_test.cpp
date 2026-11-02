#include "tracker.cpp"
#include <cassert>
#include <iostream>
#include <map>
#include <limits>
namespace {
std::map<uint32_t,std::vector<uint8_t>> memory;
WmlApi mock{};int native_batches=0,extra_batches=0,marker_draws=0,pos_fallbacks=0;bool fail_draw=false,pressed=false;
std::array<unsigned,4> color_state{11,22,33,144};std::string overlay;
std::vector<std::string> labels;
void* Pointer(uint32_t a){auto it=memory.upper_bound(a);if(it==memory.begin())return nullptr;--it;auto n=uint64_t(a)-it->first;return n<it->second.size()?it->second.data()+n:nullptr;}
uint8_t U8(uint32_t p){auto a=Pointer(p);assert(a);return *static_cast<uint8_t*>(a);}
uint16_t U16(uint32_t p){return uint16_t(U8(p))<<8|U8(p+1);}
uint32_t U32(uint32_t p){return uint32_t(U16(p))<<16|U16(p+2);}
float Float(uint32_t p){return std::bit_cast<float>(U32(p));}
void W8(uint32_t p,uint8_t v){auto a=Pointer(p);assert(a);*static_cast<uint8_t*>(a)=v;}
void W16(uint32_t p,uint16_t v){W8(p,uint8_t(v>>8));W8(p+1,uint8_t(v));}
void W32(uint32_t p,uint32_t v){W16(p,uint16_t(v>>16));W16(p+2,uint16_t(v));}
void WF(uint32_t p,float v){W32(p,std::bit_cast<uint32_t>(v));}
int Index(int i){return i==3?0:(i<3?i+1:i);}
uint64_t R(WmlContext* c,int i){return reinterpret_cast<PPCRegister*>(c)[Index(i)].u64;}
void RS(WmlContext* c,int i,uint64_t v){reinterpret_cast<PPCRegister*>(c)[Index(i)].u64=v;}
void LogFake(const WmlMod*,const char*){}
void Overlay(const char* s){overlay=s;}
int Key(int key){assert(key==VK_F7);bool result=pressed;pressed=false;return result;}
void NativeColor(WmlContext* c,uint8_t*){for(int i=0;i<4;++i)color_state[i]=unsigned(R(c,i+3));}
int Call(WmlContext* c,uint32_t fn){
    if(fn==0x8251E4E8){auto p=uint32_t(R(c,3));for(unsigned i=0;i<4;++i)W8(p+i,uint8_t(color_state[i]));}
    else if(fn==0x8263D818)RS(c,3,0x90012000);
    else if(fn==0x8263E7D8)Color(c,nullptr);
    else if(fn==0x8264EF48){auto p=uint32_t(R(c,5));unsigned n=0;while(U16(p+n*2))++n;W32(uint32_t(R(c,3)),n*12);W32(uint32_t(R(c,4)),24);}
    else if(fn==0x8264E988){std::string text;auto p=uint32_t(R(c,5));for(unsigned i=0;i<128 && U16(p+i*2);++i)text+=char(U16(p+i*2));labels.push_back(text);}
    else if(fn!=0x8263F4D0)assert(false && "Unexpected native UI function");
    return 0;
}
void NativePos(WmlContext*,uint8_t*){++pos_fallbacks;}
void NativeBatch(WmlContext* raw,uint8_t*){
    if(!custom_draw){++native_batches;return;}
    ++extra_batches;
    assert(R(raw,3)==4 && R(raw,4)==0);auto head=U32(kHead),node=head;unsigned count=0;
    do{
        assert(node && ++count<=135);auto handle=U32(node+16),i=handle&255;assert(i<missing.size());
        const auto out=uint32_t(R(raw,1))+0x3F00;
        RS(raw,3,node);RS(raw,4,out);Position(raw,nullptr);assert(R(raw,3)==1);
        for(int j=0;j<3;++j)assert(Float(out+4*j)==missing[i].pos[j]);
        RS(raw,3,255);RS(raw,4,255);RS(raw,5,255);RS(raw,6,200);Color(raw,nullptr);
        bool cd=missing[i].kind==tracker::Kind::CD;assert(color_state[0]==unsigned(cd?255:190) && color_state[3]==200);
        if(fail_draw)throw std::runtime_error("Simulated renderer failure");
        ++marker_draws;node=U32(node+4);
    }while(node!=head);
}
void NativeMap(WmlContext* c,uint8_t* b){Batch(c,b);Batch(c,b);}
}
int main(int argc,char** argv){
    assert(argc==2);auto game=std::filesystem::path(argv[1]);
    assert(tracker::NameId("123456789")==0x2DFD2D88u);assert(tracker::NameId("A")!=tracker::NameId("a"));
    std::vector<tracker::Location> sample;
    tracker::ParseScript("test.cts","#Navpoints\n$Navpoint: \"test_cd\"\n$Pos: <1, 2, 3>\n#Items\n$Item: \"sample\"\n$Item type: \"CD_collect\"\n$Start nav: \"test_cd\"\n#End\n",sample);
    assert(sample.size()==1&&sample[0].kind==tracker::Kind::CD&&sample[0].pos[2]==3);
    tracker::ParseScript("lc_tagging.cts","#Tagging Spots\n$Tagging Spot: \"sample_tag\"\n$Pos: <4 5 6>\n+Ambient\n#End",sample);
    assert(sample.size()==2&&sample[1].kind==tracker::Kind::Graffiti);
    locations=tracker::Pack(game/"dist"/"game"/"packfiles"/"misc.vpp_xbox2").Locations();assert(locations.size()==135);
    bool invalid=false;try{tracker::Validate(sample);}catch(...){invalid=true;}assert(invalid);
    for(unsigned m=0;m<4;++m){assert(tracker::Visible(tracker::Kind::CD,m)==bool(m&1));assert(tracker::Visible(tracker::Kind::Graffiti,m)==bool(m&2));}
    memory[0x82700000].resize(0x100000);memory[0x82FF0000].resize(0x10000);memory[0x83000000].resize(0x100000);memory[0x83700000].resize(0x20000);
    memory[0x90000000].resize(0x20000);memory[0x91000000].resize(0x20000);memory[0x70000000].resize(0x20000);
    mock.version=1;mock.size=sizeof(mock);mock.log=LogFake;mock.read_u8=U8;mock.read_u16=U16;mock.read_u32=U32;mock.read_f32=Float;
    mock.write_u8=W8;mock.write_u16=W16;mock.write_u32=W32;mock.write_f32=WF;mock.guest_pointer=Pointer;mock.get_r=R;mock.set_r=RS;
    mock.call=Call;mock.overlay_text=Overlay;mock.key_pressed=Key;api=&mock;static WmlMod mod{"Collectibles","test"};self=&mod;
    original_map=NativeMap;original_batch=NativeBatch;original_pos=NativePos;original_color=NativeColor;enabled=true;
    W32(0x90012000,1280);W32(0x90012004,720);WF(0x82FFB4E8,1);WF(0x82FFB4EC,1);
    W32(0x827D6D1C,1);W32(0x827D6D20,2);W32(0x827D6D14,3);W32(0x827D6D10,4);
    W32(0x8309ABEC,0x90000000);W32(0x90000000+21228,1);
    for(const auto& l:locations)if(l.kind==tracker::Kind::CD){W32(0x90000000+20988,l.id);break;}
    unsigned tag=0;for(const auto& l:locations)if(l.kind==tracker::Kind::Graffiti){auto n=0x91000000+tag*28,d=0x91010000+tag*64;W32(n+4,d);W32(d,l.id);W8(n,tag<2?0x40:0);W32(n+20,0x91000000+((tag+1)%75)*28);++tag;}
    W32(0x827E2644,0x91000000);W32(kHead,0x90002000);
    Snapshot();assert(cds_ready&&tags_ready&&cds==1&&tags==2&&missing.size()==132);
    const auto saved=memory;const auto colors=color_state;PPCContext ctx{};ctx.r1.u32=0x70018000;
    const auto before=ctx;auto raw=reinterpret_cast<WmlContext*>(&ctx);mode=3;
    Map(raw,nullptr);assert(extra_batches==1&&native_batches==2&&marker_draws==132&&pos_fallbacks==0);
    assert(U32(kHead)==0x90002000&&color_state==colors&&std::memcmp(&ctx,&before,sizeof(ctx))==0);
    assert(memory==saved); // no progress, mission-list or stack changes survive the draw
    Frame(nullptr);assert(overlay.empty());assert(std::find(labels.begin(),labels.end(),"1 / 60")!=labels.end()&&std::find(labels.begin(),labels.end(),"2 / 75")!=labels.end());
    pressed=true;Frame(nullptr);assert(mode==1);pressed=true;Frame(nullptr);assert(mode==2);pressed=true;Frame(nullptr);assert(mode==0);pressed=true;Frame(nullptr);assert(mode==3);
    const auto batches=extra_batches;mode=0;Map(raw,nullptr);assert(extra_batches==batches);
    mode=1;marker_draws=0;Map(raw,nullptr);assert(marker_draws==59);
    mode=2;marker_draws=0;Map(raw,nullptr);assert(marker_draws==73);
    mode=3;fail_draw=true;Map(raw,nullptr);assert(!enabled&&U32(kHead)==0x90002000&&memory==saved&&color_state==colors);
    enabled=true;fail_draw=false;W8(0x8370E9F6,1);auto extra=extra_batches;Map(raw,nullptr);assert(extra_batches==extra);
    std::cout<<"PASS: native CRC IDs, synthetic CTS parsing, all 135 installed locations, per-save completion filtering, four marker modes, native-font panel, once-per-map-pass drawing, map head/CPU context/stack/color restoration, failure restoration and multiplayer exclusion\n";
}
