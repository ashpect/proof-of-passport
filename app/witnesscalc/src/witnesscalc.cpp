#include "witnesscalc.h"
#include "filemaploader.hpp"
#include "calcwit.hpp"
#include "circom.hpp"
#include <nlohmann/json.hpp>
#include <sstream>
#include <memory>

namespace CIRCUIT_NAME {

using json = nlohmann::json;

Circom_Circuit* loadCircuit(const void *buffer, unsigned long buffer_size) {
    if (buffer_size % sizeof(u32) != 0) {
      throw std::runtime_error("Invalid circuit file: wrong buffer_size");
    }

    Circom_Circuit *circuit = new Circom_Circuit;

    u8* bdata = (u8*)buffer;

    circuit->InputHashMap = new HashSignalInfo[get_size_of_input_hashmap()];
    uint dsize = get_size_of_input_hashmap()*sizeof(HashSignalInfo);
    if (buffer_size < dsize) {
        throw std::runtime_error("Invalid circuit file: buffer_size <= dsize");
    }
    memcpy((void *)(circuit->InputHashMap), (void *)bdata, dsize);

    circuit->witness2SignalList = new u64[get_size_of_witness()];
    uint inisize = dsize;
    dsize = get_size_of_witness()*sizeof(u64);
    if (buffer_size < dsize + inisize) {
        throw std::runtime_error("Invalid circuit file: buffer_size <= dsize + inisize");
    }
    memcpy((void *)(circuit->witness2SignalList), (void *)(bdata+inisize), dsize);

    circuit->circuitConstants = new FrElement[get_size_of_constants()];
    if (get_size_of_constants()>0) {
      inisize += dsize;
      dsize = get_size_of_constants()*sizeof(FrElement);
      if (buffer_size < dsize + inisize) {
        throw std::runtime_error("Invalid circuit file: buffer_size <= dsize + inisize");
      }
      memcpy((void *)(circuit->circuitConstants), (void *)(bdata+inisize), dsize);
    }

    std::map<u32,IODefPair> templateInsId2IOSignalInfo1;
    if (get_size_of_io_map()>0) {
      u32 index[get_size_of_io_map()];
      inisize += dsize;
      dsize = get_size_of_io_map()*sizeof(u32);
      if (buffer_size < dsize + inisize) {
        throw std::runtime_error("Invalid circuit file: buffer_size <= dsize + inisize");
      }
      memcpy((void *)index, (void *)(bdata+inisize), dsize);
      inisize += dsize;
      if (inisize % sizeof(u32) != 0) {
        throw std::runtime_error("Invalid circuit file: wrong inisize");
      }
      if (buffer_size <= inisize) {
          throw std::runtime_error("Invalid circuit file: buffer_size <= inisize");
      }
      u32 dataiomap[(buffer_size-inisize)/sizeof(u32)];
      memcpy((void *)dataiomap, (void *)(bdata+inisize), buffer_size-inisize);
      u32* pu32 = dataiomap;

      for (int i = 0; i < get_size_of_io_map(); i++) {
        u32 n = *pu32;
        IODefPair p;
        p.len = n;
        IODef defs[n];
        pu32 += 1;
        for (u32 j = 0; j <n; j++){
          defs[j].offset=*pu32;
          u32 len = *(pu32+1);
          defs[j].len = len;
          defs[j].lengths = new u32[len];
          memcpy((void *)defs[j].lengths,(void *)(pu32+2),len*sizeof(u32));
          pu32 += len + 2;
        }
        p.defs = (IODef*)calloc(10, sizeof(IODef));
        for (u32 j = 0; j < p.len; j++){
          p.defs[j] = defs[j];
        }
        templateInsId2IOSignalInfo1[index[i]] = p;
      }
    }
    circuit->templateInsId2IOSignalInfo = std::move(templateInsId2IOSignalInfo1);

    return circuit;
}

bool check_valid_number(std::string & s, uint base){
  bool is_valid = true;
  if (base == 16){
    for (uint i = 0; i < s.size(); i++){
      is_valid &= (
        ('0' <= s[i] && s[i] <= '9') ||
        ('a' <= s[i] && s[i] <= 'f') ||
        ('A' <= s[i] && s[i] <= 'F')
      );
    }
  } else{
    for (uint i = 0; i < s.size(); i++){
      is_valid &= ('0' <= s[i] && s[i] < char(int('0') + base));
    }
  }
  return is_valid;
}

void json2FrElements (json val, std::vector<FrElement> & vval){
  if (!val.is_array()) {
    FrElement v;
    std::string s_aux, s;
    uint base;
    if (val.is_string()) {
      s_aux = val.get<std::string>();
      std::string possible_prefix = s_aux.substr(0, 2);
      if (possible_prefix == "0b" || possible_prefix == "0B"){
        s = s_aux.substr(2, s_aux.size() - 2);
        base = 2;
      } else if (possible_prefix == "0o" || possible_prefix == "0O"){
        s = s_aux.substr(2, s_aux.size() - 2);
        base = 8;
      } else if (possible_prefix == "0x" || possible_prefix == "0X"){
        s = s_aux.substr(2, s_aux.size() - 2);
        base = 16;
      } else{
        s = s_aux;
        base = 10;
      }
      if (!check_valid_number(s, base)){
        std::ostringstream errStrStream;
        errStrStream << "Invalid number in JSON input: " << s_aux << "\n";
          throw std::runtime_error(errStrStream.str() );
      }
    } else if (val.is_number()) {
        double vd = val.get<double>();
        std::stringstream stream;
        stream << std::fixed << std::setprecision(0) << vd;
        s = stream.str();
        base = 10;
    } else {
        throw std::runtime_error("Invalid JSON type");
    }
    Fr_str2element (&v, s.c_str(), base);
    vval.push_back(v);
  } else {
    for (uint i = 0; i < val.size(); i++) {
      json2FrElements (val[i], vval);
    }
  }
}

void loadJson(Circom_CalcWit *ctx, const char *json_buffer, unsigned long buffer_size) {

  json j = json::parse(json_buffer, json_buffer + buffer_size);

  u64 nItems = j.size();
  // printf("Items : %llu\n",nItems);
  if (nItems == 0){
    ctx->tryRunCircuit();
  }
  for (json::iterator it = j.begin(); it != j.end(); ++it) {
    // std::cout << it.key() << " => " << it.value() << '\n';
    u64 h = fnv1a(it.key());
    std::vector<FrElement> v;
    json2FrElements(it.value(),v);
    uint signalSize = ctx->getInputSignalSize(h);
    if (v.size() < signalSize) {
	std::ostringstream errStrStream;
	errStrStream << "Error loading signal " << it.key() << ": Not enough values\n";
	throw std::runtime_error(errStrStream.str() );
    }
    if (v.size() > signalSize) {
	std::ostringstream errStrStream;
	errStrStream << "Error loading signal " << it.key() << ": Too many values\n";
	throw std::runtime_error(errStrStream.str() );
    }
    for (uint i = 0; i<v.size(); i++){
      try {
        // std::cout << it.key() << "," << i << " => " << Fr_element2str(&(v[i])) << '\n';
        ctx->setInputSignal(h,i,v[i]);
      } catch (std::runtime_error e) {
        std::ostringstream errStrStream;
        errStrStream << "Error setting signal: " << it.key() << "\n" << e.what();
        throw std::runtime_error(errStrStream.str() );
      }
    }
  }
}

unsigned long getBinWitnessSize() {

     uint Nwtns = get_size_of_witness();

     return 44 + Fr_N64*8 * (Nwtns + 1);
}

char *appendBuffer(char *buffer, const void *src, unsigned long src_size) {

    memcpy(buffer, src, src_size);
    return buffer + src_size;
}

char *appendBuffer(char *buffer, const u32 src) {

    return appendBuffer(buffer, &src, 4);
}

char *appendBuffer(char *buffer, const u64 src) {

    return appendBuffer(buffer, &src, 8);
}

char *appendBuffer(char *buffer, const FrRawElement src) {

    return appendBuffer(buffer, src, Fr_N64*8);
}

void storeBinWitness(Circom_CalcWit *ctx, char *buffer) {

     buffer = appendBuffer(buffer,  "wtns", 4);

     u32 version = 2;
     buffer = appendBuffer(buffer, version);

     u32 nSections = 2;
     buffer = appendBuffer(buffer, nSections);

     // Header
     u32 idSection1 = 1;
     buffer = appendBuffer(buffer, idSection1);

     u32 n8 = Fr_N64*8;

     u64 idSection1length = 8 + n8;
     buffer = appendBuffer(buffer, idSection1length);

     buffer = appendBuffer(buffer, n8);

     buffer = appendBuffer(buffer, Fr_q.longVal);

     uint Nwtns = get_size_of_witness();

     u32 nVars = (u32)Nwtns;
     buffer = appendBuffer(buffer, nVars);

     // Data
     u32 idSection2 = 2;
     buffer = appendBuffer(buffer, idSection2);

     u64 idSection2length = (u64)n8*(u64)Nwtns;
     buffer = appendBuffer(buffer, idSection2length);

     FrElement v;

     for (int i=0;i<Nwtns;i++) {
         ctx->getWitness(i, &v);
         Fr_toLongNormal(&v, &v);
         buffer = appendBuffer(buffer, v.longVal);
     }
}

int witnesscalc(
    const char *circuit_buffer,  unsigned long  circuit_size,
    const char *json_buffer,     unsigned long  json_size,
    char       *wtns_buffer,     unsigned long *wtns_size,
    char       *error_msg,       unsigned long  error_msg_maxsize)
{
    unsigned long witnessSize = getBinWitnessSize();

    if (*wtns_size < witnessSize) {
        *wtns_size = witnessSize;
        return WITNESSCALC_ERROR_SHORT_BUFFER;
    }

    try {

        std::unique_ptr<Circom_Circuit> circuit(loadCircuit(circuit_buffer, circuit_size));

        std::unique_ptr<Circom_CalcWit> ctx(new Circom_CalcWit(circuit.get()));

        loadJson(ctx.get(), json_buffer, json_size);

        if (ctx->getRemaingInputsToBeSet() != 0) {
            std::stringstream stream;
            stream << "Not all inputs have been set. Only "
                   << get_main_input_signal_no()-ctx->getRemaingInputsToBeSet()
                   << " out of " << get_main_input_signal_no();

            strncpy(error_msg, stream.str().c_str(), error_msg_maxsize);
            return WITNESSCALC_ERROR;
        }

        storeBinWitness(ctx.get(), wtns_buffer);
        *wtns_size = witnessSize;

    } catch (std::exception& e) {

        if (error_msg) {
            strncpy(error_msg, e.what(), error_msg_maxsize);
        }
        return WITNESSCALC_ERROR;

    } catch (std::exception *e) {

        if (error_msg) {
            strncpy(error_msg, e->what(), error_msg_maxsize);
        }
        delete e;
        return WITNESSCALC_ERROR;

    } catch (...) {
        if (error_msg) {
            strncpy(error_msg, "unknown error", error_msg_maxsize);
        }
        return WITNESSCALC_ERROR;
    }

    return WITNESSCALC_OK;
}

int witnesscalc_from_dat_file(
        const char *dat_fname,
        const char *json_buffer,     unsigned long  json_size,
        char       *wtns_buffer,     unsigned long *wtns_size,
        char       *error_msg,       unsigned long  error_msg_maxsize)
{

    std::string s(dat_fname);
    FileMapLoader dat(dat_fname);
    return witnesscalc(dat.buffer, dat.size, json_buffer,
                       json_size, wtns_buffer, wtns_size,
                       error_msg, error_msg_maxsize);
}


} // namespace
