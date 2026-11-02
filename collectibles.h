#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <zlib.h>

namespace tracker {
enum class Kind { CD, Graffiti };
struct Location {Kind kind;uint32_t id;std::array<float,3> pos;std::string name;};
inline uint32_t NameId(const std::string& name){
    uint32_t crc=0;
    for(unsigned char c:name){crc^=c;for(int bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xEDB88320u:0u);}
    return crc;
}
inline std::string Trim(std::string s){auto a=s.find_first_not_of(" \t\r\n");if(a==std::string::npos)return {};auto b=s.find_last_not_of(" \t\r\n");return s.substr(a,b-a+1);}
inline std::string Quoted(const std::string& line){auto a=line.find('"');if(a==std::string::npos)return {};auto b=line.find('"',a+1);return b==std::string::npos?std::string():line.substr(a+1,b-a-1);}
inline bool Position(std::string line,std::array<float,3>& p){
    auto a=line.find('<'),b=line.find('>');if(a==std::string::npos||b==std::string::npos||b<=a)return false;
    line=line.substr(a+1,b-a-1);for(char& c:line)if(c==',')c=' ';
    std::istringstream in(line);if(!(in>>p[0]>>p[1]>>p[2]))return false;
    for(float n:p)if(!std::isfinite(n)||std::abs(n)>100000)return false;
    return true;
}
inline void ParseScript(const std::string& file,const std::string& text,std::vector<Location>& out){
    struct Item{std::string name,nav;bool cd=false;};
    std::unordered_map<std::string,std::array<float,3>> navs;
    std::vector<Item> items;
    std::string section,nav,tag;Item item;std::array<float,3> tag_pos{};bool has_tag_pos=false,ambient=false;
    const bool tagging=file=="lc_tagging.cts"||file=="vk_tagging.cts"||file=="wr_tagging.cts";
    auto flush_item=[&]{if(!item.name.empty())items.push_back(item);item={};};
    auto flush_tag=[&]{if(tagging&&!tag.empty()&&has_tag_pos&&ambient)out.push_back({Kind::Graffiti,NameId(tag),tag_pos,tag});tag.clear();has_tag_pos=false;ambient=false;};
    std::istringstream lines(text);std::string line;
    while(std::getline(lines,line)){
        line=Trim(line);if(line.empty()||line.starts_with("//"))continue;
        if(line[0]=='#'){flush_item();flush_tag();section=line;nav.clear();continue;}
        if(section=="#Navpoints"){
            if(line.starts_with("$Navpoint:"))nav=Quoted(line);
            else if(line.starts_with("$Pos:")&&!nav.empty()){std::array<float,3> p;if(Position(line,p))navs[nav]=p;}
        }else if(section=="#Items"){
            if(line.starts_with("$Item:")){flush_item();item.name=Quoted(line);}
            else if(line.starts_with("$Item type:"))item.cd=Quoted(line)=="CD_collect";
            else if(line.starts_with("$Start nav:"))item.nav=Quoted(line);
        }else if(tagging&&section=="#Tagging Spots"){
            if(line.starts_with("$Tagging Spot:")){flush_tag();tag=Quoted(line);}
            else if(line.starts_with("$Pos:"))has_tag_pos=Position(line,tag_pos);
            else if(line.starts_with("+Ambient"))ambient=true;
        }
    }
    flush_item();flush_tag();
    for(const auto& i:items)if(i.cd){auto n=navs.find(i.nav);if(n==navs.end())throw std::runtime_error("CD navpoint missing in "+file);out.push_back({Kind::CD,NameId(i.name),n->second,i.name});}
}
inline void Validate(const std::vector<Location>& locations){
    unsigned cds=0,tags=0;std::unordered_set<uint32_t> ids;
    for(const auto& l:locations){if(!ids.insert(l.id).second)throw std::runtime_error("Duplicate collectible ID");if(l.kind==Kind::CD)++cds;else ++tags;}
    if(cds!=60||tags!=75)throw std::runtime_error("Expected 60 CDs and 75 graffiti locations; game data differs");
}
class Pack {
    struct Entry{std::string name;uint32_t offset,size,stored;};
    std::filesystem::path path;std::vector<Entry> entries;uint32_t flags=0,data_at=0,blob_size=0,stored_size=0;
    std::vector<uint8_t> blob;
    static uint32_t Align(uint32_t n){return (n+2047)&~2047u;}
    static uint32_t Be(const std::vector<uint8_t>& b,size_t p){if(p+4>b.size())throw std::runtime_error("Truncated packfile");return uint32_t(b[p])<<24|uint32_t(b[p+1])<<16|uint32_t(b[p+2])<<8|b[p+3];}
    static std::vector<uint8_t> ReadAt(std::ifstream& in,uint64_t offset,size_t size){std::vector<uint8_t> b(size);in.seekg(offset);if(!in.read(reinterpret_cast<char*>(b.data()),size))throw std::runtime_error("Cannot read packfile");return b;}
    static std::vector<uint8_t> Inflate(const std::vector<uint8_t>& data,uint32_t size){
        if(size>128*1024*1024)throw std::runtime_error("Packfile entry too large");
        std::vector<uint8_t> out(size);z_stream stream{};
        if(inflateInit(&stream)!=Z_OK)throw std::runtime_error("Cannot initialize decompressor");
        stream.next_in=const_cast<Bytef*>(data.data());stream.avail_in=static_cast<uInt>(data.size());stream.next_out=out.data();stream.avail_out=size;
        inflate(&stream,Z_FINISH);const auto produced=stream.total_out;inflateEnd(&stream);
        if(produced!=size)throw std::runtime_error("Cannot decompress packfile entry");return out;
    }
public:
    explicit Pack(const std::filesystem::path& p):path(p){
        std::ifstream in(path,std::ios::binary);auto h=ReadAt(in,0,0x800);
        if(Be(h,0)!=0x51890ACE||Be(h,4)!=3)throw std::runtime_error("Unsupported packfile format");
        flags=Be(h,0x14C);auto count=Be(h,0x154),directory=Be(h,0x15C),names_size=Be(h,0x160);
        if(count>100000||uint64_t(count)*28>directory||names_size>16*1024*1024)throw std::runtime_error("Invalid packfile directory");
        auto names_at=Align(0x800+directory);data_at=Align(names_at+names_size);blob_size=Be(h,0x164);stored_size=Be(h,0x168);
        auto records=ReadAt(in,0x800,count*28),names=ReadAt(in,names_at,names_size);uint32_t stored=data_at;
        for(uint32_t i=0;i<count;++i){auto at=Be(records,i*28);if(at>=names.size())throw std::runtime_error("Invalid packfile name");
            auto end=at;while(end<names.size()&&names[end])++end;if(end==names.size())throw std::runtime_error("Unterminated packfile name");
            entries.push_back({std::string(reinterpret_cast<char*>(names.data()+at),end-at),(flags&2)?Be(records,i*28+8):((flags&1)?stored:data_at+Be(records,i*28+8)),Be(records,i*28+16),Be(records,i*28+20)});
            if(flags&1)stored=Align(stored+entries.back().stored);
        }
    }
    std::vector<Location> Locations(){
        std::vector<Location> out;std::ifstream in(path,std::ios::binary);
        if(flags&2){auto raw=ReadAt(in,data_at,(flags&1)?stored_size:blob_size);blob=(flags&1)?Inflate(raw,blob_size):std::move(raw);}
        for(const auto& e:entries){if(!e.name.ends_with(".cts"))continue;if(e.size>8*1024*1024)throw std::runtime_error("Script entry too large");
            std::vector<uint8_t> bytes;
            if(flags&2){auto offset=e.offset;
                if(offset+uint64_t(e.size)>blob.size())throw std::runtime_error("Invalid condensed entry");bytes.assign(blob.begin()+offset,blob.begin()+offset+e.size);
            }else{auto raw=ReadAt(in,e.offset,(flags&1)?e.stored:e.size);bytes=(flags&1)?Inflate(raw,e.size):std::move(raw);}
            ParseScript(e.name,std::string(bytes.begin(),bytes.end()),out);
        }
        Validate(out);return out;
    }
};
inline bool Visible(Kind kind,unsigned mode){return (mode&(kind==Kind::CD?1u:2u))!=0;}
}
