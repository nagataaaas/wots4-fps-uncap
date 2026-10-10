// まだ中身のない Mod の入口。ModLoader から呼ばれ、何もせずに受け入れる。
#include <wots4/mod_api.h>

extern "C" BOOL WINAPI ModInitialize(const wots4::ModContext* context) {
    if(!context||context->size<sizeof(*context)||context->version!=wots4::modApiVersion||!context->host)
        return FALSE;
    if(context->host->log)
        context->host->log(context->module,"not implemented yet");
    return TRUE;
}
