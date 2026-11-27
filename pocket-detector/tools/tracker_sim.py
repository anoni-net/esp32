# 用 Mac 的藍牙模擬追蹤器的發聲服務，記下 ESP32 寫入的每一個 byte，驗證偵測器的發聲流程。
#
# 用法（需要 bless，例如 uv run --with bless python tracker_sim.py dult 60）：
#   python tracker_sim.py dult|dultrefuse|airtag|findmy|pebble 秒數
# 模擬開始後，在偵測器的序列埠送出 soundtest。偵測器會主動掃描、找到廣播這個服務的
# 裝置，執行發聲流程，結果印在兩邊。dult 會回覆成功與播放完畢，dultrefuse 回覆拒絕。
import asyncio, sys, time
from bless import BlessServer, GATTCharacteristicProperties as P, GATTAttributePermissions as A

def full(u16):
    return f"0000{u16:04x}-0000-1000-8000-00805f9b34fb"

PROTOS = {
    "dultrefuse": ("15190001-12f4-c226-88ed-2ac5579f2a85", "8e0c0001-1d68-fb92-bf61-48377421680e",
                   P.write | P.indicate),
    "dult": ("15190001-12f4-c226-88ed-2ac5579f2a85", "8e0c0001-1d68-fb92-bf61-48377421680e",
             P.write | P.indicate),
    "airtag": ("7dfc9000-7d1c-4951-86aa-8d9728f8d66c", "7dfc9001-7d1c-4951-86aa-8d9728f8d66c", P.write),
    "findmy": (full(0xFD44), "4f860003-943b-49ef-bed4-2f730304427a", P.write | P.indicate),
    "pebble": (full(0xFA25), full(0x2C02), P.write | P.write_without_response | P.notify),
}

proto, secs = sys.argv[1], float(sys.argv[2])
svc, chr_, props = PROTOS[proto]
writes = []
server = None
loop = None


def on_write(characteristic, value, **kwargs):
    v = bytes(value)
    writes.append(v.hex())
    print(time.strftime("%H:%M:%S"), "收到寫入", v.hex(), flush=True)
    if proto == "dultrefuse" and v == b"\x00\x03":
        # 主人在附近時，規格規定回 Invalid_command（0xFFFF）
        loop.call_soon_threadsafe(loop.call_later, 0.3, indicate, bytes([0x02, 0x03, 0x00, 0x03, 0xFF, 0xFF]))
    if proto == "dult" and v == b"\x00\x03":
        # Command_Response：opcode 0x0302、對應的 0x0300、狀態 0x0000，接著 Sound_Completed 0x0303
        # 寫入的回呼在 CoreBluetooth 的執行緒，要用執行緒安全的方式排程
        loop.call_soon_threadsafe(loop.call_later, 0.3, indicate, bytes([0x02, 0x03, 0x00, 0x03, 0x00, 0x00]))
        loop.call_soon_threadsafe(loop.call_later, 3.0, indicate, bytes([0x03, 0x03]))


def indicate(v):
    c = server.get_characteristic(chr_)
    c.value = bytearray(v)
    server.update_value(svc, chr_)
    print(time.strftime("%H:%M:%S"), "送出 indication", v.hex(), flush=True)


async def main():
    global server, loop
    loop = asyncio.get_running_loop()
    server = BlessServer(name="SimTrk", loop=loop)
    server.read_request_func = lambda c, **k: c.value
    server.write_request_func = on_write
    await server.add_new_service(svc)
    await server.add_new_characteristic(svc, chr_, props, None, A.readable | A.writeable)
    await server.start()
    print(time.strftime("%H:%M:%S"), "開始模擬", proto, svc, flush=True)
    await asyncio.sleep(secs)
    await server.stop()
    print("結果", proto, writes, flush=True)

asyncio.run(main())
