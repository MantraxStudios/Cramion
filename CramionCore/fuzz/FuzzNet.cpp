// Fuzzing de los paquetes de red (net/Network.h): los valores de los
// mensajes (decodeValue, tablas anidadas), las posiciones absolutas y la
// sesion entera (NetworkSession::receiveForTest) haciendo de servidor y de
// cliente. Es lo que cualquier jugador puede mandar al servidor dedicado.
//
// Entrada: 1 byte de papel (par = servidor, impar = cliente) y despues
// paquetes [u16 largo][bytes] que llegan uno detras de otro (asi un Spawn
// puede crear el objeto que mueve el Transform siguiente).

#include "FuzzCommon.h"

#include "CramionCore/net/Network.h"

using namespace cramion;

namespace {

void put16(std::string& out, std::uint16_t v) {
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>(v >> 8));
}
void put32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void putStr(std::string& out, const std::string& s) {
    put32(out, static_cast<std::uint32_t>(s.size()));
    out += s;
}
void addPacket(std::string& input, const std::string& packet) {
    put16(input, static_cast<std::uint16_t>(packet.size()));
    input += packet;
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
    const std::filesystem::path seeds = fuzz::seedsFolder(argc, argv);
    if (seeds.empty()) return 0;
    std::error_code e;
    std::filesystem::create_directories(seeds, e);
    // Cliente: bienvenida, alguien entra, aparece un objeto, se mueve, cambia
    // una variable, un mensaje con una tabla y una escena nueva.
    net::NetValue table;
    table.type = net::NetValue::Type::Table;
    table.entries.emplace_back(net::NetValue::str("vida"), net::NetValue::num(100.0));
    table.entries.emplace_back(net::NetValue::num(1.0), net::NetValue::vec3({1.0f, 2.0f, 3.0f}));
    std::string client(1, '\x01');
    {
        std::string p(1, '\x01');  // kWelcome
        put32(p, 7);
        put32(p, 1);
        put32(p, 1);
        put32(p, net::kProtocolVersion);
        addPacket(client, p);
    }
    {
        std::string p(1, '\x02');  // kJoin
        put32(p, 9);
        addPacket(client, p);
    }
    {
        std::string p(1, '\x05');  // kSpawn
        put32(p, 3);
        put32(p, 1);
        putStr(p, "Prefabs/Jugador.crprefab");
        net::encodePosition(p, net::NetPosition{10.0, 2.0, -5.0});
        for (int i = 0; i < 3; ++i) put32(p, 0);
        put32(p, 0x3f800000u);  // w = 1
        addPacket(client, p);
    }
    {
        std::string p(1, '\x04');  // kUser
        put32(p, 1);
        put32(p, 0xffffffffu);
        putStr(p, "danio");
        net::encodeValue(p, table);
        addPacket(client, p);
    }
    fuzz::writeFile(seeds / "cliente.bin", client);
    // Servidor: un cliente manda un mensaje y una variable.
    std::string server(1, '\x00');
    {
        std::string p(1, '\x04');
        put32(p, 5);
        put32(p, 1);
        putStr(p, "hola");
        net::encodeValue(p, net::NetValue::str("mundo"));
        addPacket(server, p);
    }
    fuzz::writeFile(seeds / "servidor.bin", server);
    std::string value;
    net::encodeValue(value, table);
    fuzz::writeFile(seeds / "valor.bin", std::string(1, '\x01') + value);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string all(reinterpret_cast<const char*>(data), size);
    // Valores y posiciones sueltos desde cualquier punto.
    {
        std::size_t at = 0;
        net::NetValue v;
        while (at < all.size() && net::decodeValue(all, at, v)) {
            std::string again;
            net::encodeValue(again, v);
        }
        at = 0;
        net::NetPosition p;
        net::decodePosition(all, at, p);
    }
    if (size < 1) return 0;
    net::NetworkSession session;
    const net::NetRole role = (data[0] & 1) == 0 ? net::NetRole::Server : net::NetRole::Client;
    std::size_t at = 1;
    int packets = 0;
    while (at + 2 <= size && packets++ < 64) {
        const std::size_t n = static_cast<std::size_t>(data[at]) | (static_cast<std::size_t>(data[at + 1]) << 8);
        at += 2;
        const std::size_t take = std::min(n, size - at);
        session.receiveForTest(role, 2, all.substr(at, take));
        at += take;
    }
    session.takeEvents();
    return 0;
}
