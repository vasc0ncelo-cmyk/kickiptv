#include "../src/json.hpp"
#include <cassert>
#include <iostream>

int main() {
    Json j;
    assert(jsonParse("{\"user_info\":{\"auth\":1,\"status\":\"Active\"},\"n\":12.5,\"ok\":true,\"x\":null}", j));
    assert(j.get("user_info")->num("auth") == 1 && j.get("user_info")->str("status") == "Active");
    assert(j.num("n") == 12 && j.str("ok") == "1" && j.str("zzz", "padrao") == "padrao");
    // string/numero misturados (paineis fazem isso)
    assert(jsonParse("{\"stream_id\":585332,\"series_id\":\"77\",\"container_extension\":\"mp4\"}", j));
    assert(j.str("stream_id") == "585332" && j.str("series_id") == "77" && j.num("series_id") == 77);
    // escapes: \u00e9, barra escapada, emoji (par substituto)
    assert(jsonParse("{\"name\":\"S\\u00e9rie \\/ A\\\"B\\\" \\ud83d\\ude00\"}", j));
    assert(j.str("name") == "S\xC3\xA9rie / A\"B\" \xF0\x9F\x98\x80");
    // array grande, um por vez
    {
        std::string t = "[";
        for (int i = 0; i < 20000; i++) { if (i) t += ","; t += "{\"name\":\"Filme " + std::to_string(i) + "\",\"stream_id\":" + std::to_string(i) + ",\"extra\":{\"a\":[1,2,3],\"b\":\"x\"}}"; }
        t += "]";
        size_t n = 0; long long sum = 0;
        assert(jsonForEach(t, [&](const Json &e) { n++; sum += e.num("stream_id"); }) == 20000 && n == 20000 && sum == 199990000LL);
        // cortado no meio: entrega os completos
        std::string cut = t.substr(0, t.size() / 2);
        size_t m = jsonForEach(cut, [&](const Json &) {});
        assert(m > 5000 && m < 12000);
    }
    // nao e array / vazio / lixo
    assert(jsonForEach("{\"user_info\":{}}", [](const Json &) {}) == 0);
    assert(jsonForEach("[]", [](const Json &) {}) == 0);
    assert(jsonForEach("<html>erro</html>", [](const Json &) {}) == 0);
    assert(jsonForEach("", [](const Json &) {}) == 0);
    // episodios do get_series_info: temporadas como chaves de objeto
    assert(jsonParse("{\"episodes\":{\"1\":[{\"id\":\"10\",\"episode_num\":1,\"title\":\"A\",\"container_extension\":\"mkv\"}],\"2\":[{\"id\":\"11\",\"episode_num\":\"3\"}]}}", j));
    const Json *eps = j.get("episodes"); assert(eps && eps->isObj() && eps->obj.size() == 2 && eps->obj[0].first == "1");
    assert(eps->obj[0].second.arr[0].str("container_extension") == "mkv" && eps->obj[1].second.arr[0].num("episode_num") == 3);
    // painel devolve [] no lugar de {} quando nao ha episodios
    assert(jsonParse("{\"episodes\":[]}", j) && j.get("episodes")->isArr());
    std::cout << "OK\n";
}
