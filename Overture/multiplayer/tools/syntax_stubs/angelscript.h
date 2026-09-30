// syntax-check stub
#pragma once
#include <cstddef>
#include <string>
typedef unsigned int asUINT;
typedef unsigned long asPWORD;
typedef unsigned char asBYTE;
typedef unsigned short asWORD;
typedef unsigned int asDWORD;
typedef unsigned long long asQWORD;
typedef long long asINT64;
enum asERetCodes { asSUCCESS = 0, asERROR = -1 };
enum asEMsgType { asMSGTYPE_ERROR, asMSGTYPE_WARNING, asMSGTYPE_INFORMATION };
enum asEContextState { asEXECUTION_FINISHED, asEXECUTION_SUSPENDED, asEXECUTION_ABORTED, asEXECUTION_EXCEPTION, asEXECUTION_PREPARED, asEXECUTION_UNINITIALIZED, asEXECUTION_ACTIVE, asEXECUTION_ERROR };
enum asECallConvTypes { asCALL_CDECL, asCALL_STDCALL, asCALL_THISCALL, asCALL_CDECL_OBJLAST, asCALL_CDECL_OBJFIRST, asCALL_GENERIC };
enum asEBehaviours { asBEHAVE_CONSTRUCT, asBEHAVE_DESTRUCT, asBEHAVE_FACTORY, asBEHAVE_ADDREF, asBEHAVE_RELEASE, asBEHAVE_ASSIGNMENT, asBEHAVE_ADD, asBEHAVE_INDEX };
enum asEObjTypeFlags { asOBJ_REF = 1, asOBJ_VALUE = 2, asOBJ_CLASS = 4, asOBJ_APP_CLASS = 8, asOBJ_APP_CLASS_CDAK = 16, asOBJ_POD = 32, asOBJ_APP_PRIMITIVE = 64 };
enum asEGMFlags { asGM_ONLY_IF_EXISTS, asGM_CREATE_IF_NOT_EXISTS, asGM_ALWAYS_CREATE };
struct asSMessageInfo { const char *section; int row; int col; asEMsgType type; const char *message; };
struct asSFuncPtr { void *ptr; int flag; };
class asIScriptEngine;
class asIScriptModule;
class asIScriptContext;
class asIScriptFunction;
class asIScriptGeneric;
class asIObjectType;
class asIScriptFunction { public: virtual ~asIScriptFunction(){} virtual const char *GetName() const = 0; virtual const char *GetDeclaration(bool a=true,bool b=false,bool c=false) const = 0; virtual int GetId() const = 0; virtual const char *GetModuleName() const = 0; };
class asIScriptModule {
public:
	virtual ~asIScriptModule(){}
	virtual int AddScriptSection(const char *name, const char *code, size_t len = 0, int line = 0) = 0;
	virtual int Build() = 0;
	virtual int GetFunctionIdByName(const char *name) = 0;
	virtual int GetFunctionIdByDecl(const char *decl) = 0;
	virtual asIScriptFunction *GetFunctionByName(const char *name) = 0;
	virtual asIScriptFunction *GetFunctionByDecl(const char *decl) = 0;
	virtual asIScriptFunction *GetFunctionDescriptorById(int id) = 0;
	virtual asIScriptFunction *GetFunctionByIndex(asUINT i) = 0;
	virtual asUINT GetFunctionCount() const = 0;
	virtual int Discard() = 0;
	virtual const char *GetName() const = 0;
};
class asIScriptContext {
public:
	virtual ~asIScriptContext(){}
	virtual int AddRef() = 0; virtual int Release() = 0;
	virtual int Prepare(int funcId) = 0;
	virtual int Prepare(asIScriptFunction *f) = 0;
	virtual int Execute() = 0;
	virtual int Abort() = 0;
	virtual int GetState() const = 0;
	virtual int SetArgDWord(asUINT arg, asDWORD v) = 0;
	virtual int SetArgQWord(asUINT arg, asQWORD v) = 0;
	virtual int SetArgFloat(asUINT arg, float v) = 0;
	virtual int SetArgDouble(asUINT arg, double v) = 0;
	virtual int SetArgByte(asUINT arg, asBYTE v) = 0;
	virtual int SetArgWord(asUINT arg, asWORD v) = 0;
	virtual int SetArgAddress(asUINT arg, void *v) = 0;
	virtual int SetArgObject(asUINT arg, void *v) = 0;
	virtual asDWORD GetReturnDWord() = 0;
	virtual float GetReturnFloat() = 0;
	virtual void *GetReturnObject() = 0;
	virtual int GetExceptionLineNumber(int *col = 0, const char **section = 0) = 0;
	virtual int GetExceptionFunction() = 0;
	virtual const char *GetExceptionString() = 0;
	virtual int SetException(const char *s) = 0;
	virtual int GetCurrentLineNumber(int *col = 0) = 0;
	virtual int GetCurrentFunction() = 0;
	virtual asIScriptEngine *GetEngine() = 0;
	virtual asIScriptFunction *GetExceptionFunctionPtr() = 0;
	virtual asIScriptFunction *GetFunction(asUINT idx = 0) = 0;
	virtual int GetLineNumber(asUINT idx = 0, int *col = 0, const char **section = 0) = 0;
};
class asIScriptGeneric {
public:
	virtual ~asIScriptGeneric(){}
	virtual asIScriptEngine *GetEngine() = 0;
	virtual void *GetArgAddress(asUINT a) = 0;
	virtual void *GetArgObject(asUINT a) = 0;
	virtual asDWORD GetArgDWord(asUINT a) = 0;
	virtual float GetArgFloat(asUINT a) = 0;
	virtual void *GetAddressOfArg(asUINT a) = 0;
	virtual void *GetAddressOfReturnLocation() = 0;
	virtual int SetReturnDWord(asDWORD v) = 0;
	virtual int SetReturnFloat(float v) = 0;
	virtual int SetReturnObject(void *v) = 0;
	virtual int SetReturnAddress(void *v) = 0;
	virtual void *GetObject() = 0;
};
class asIScriptEngine {
public:
	virtual ~asIScriptEngine(){}
	virtual int AddRef() = 0; virtual int Release() = 0;
	virtual int SetMessageCallback(const asSFuncPtr &cb, void *obj, asDWORD conv) = 0;
	virtual int ClearMessageCallback() = 0;
	virtual int RegisterGlobalFunction(const char *decl, const asSFuncPtr &fp, asDWORD conv) = 0;
	virtual int RegisterGlobalProperty(const char *decl, void *ptr) = 0;
	virtual int RegisterObjectType(const char *name, int size, asDWORD flags) = 0;
	virtual int RegisterObjectProperty(const char *obj, const char *decl, int off) = 0;
	virtual int RegisterObjectMethod(const char *obj, const char *decl, const asSFuncPtr &fp, asDWORD conv) = 0;
	virtual int RegisterObjectBehaviour(const char *obj, asEBehaviours b, const char *decl, const asSFuncPtr &fp, asDWORD conv) = 0;
	virtual int RegisterStringFactory(const char *type, const asSFuncPtr &fp, asDWORD conv) = 0;
	virtual int RegisterEnum(const char *type) = 0;
	virtual int RegisterEnumValue(const char *type, const char *name, int v) = 0;
	virtual int RegisterTypedef(const char *type, const char *decl) = 0;
	virtual int RegisterInterface(const char *name) = 0;
	virtual asIScriptModule *GetModule(const char *name, asEGMFlags flag = asGM_ONLY_IF_EXISTS) = 0;
	virtual int DiscardModule(const char *name) = 0;
	virtual asIScriptContext *CreateContext() = 0;
	virtual asIScriptFunction *GetFunctionById(int id) const = 0;
	virtual asIScriptFunction *GetFunctionDescriptorById(int id) = 0;
	virtual int GetTypeIdByDecl(const char *decl) = 0;
	virtual asIObjectType *GetObjectTypeById(int id) = 0;
	virtual asIObjectType *GetObjectTypeByName(const char *name) = 0;
	virtual void *CreateScriptObject(int typeId) = 0;
	virtual int ExecuteString(const char *module, const char *script, asIScriptContext **ctx = 0, asDWORD flags = 0) = 0;
	virtual int AddScriptSection(const char *module, const char *name, const char *code, size_t len = 0, int line = 0, bool mkcopy = true) = 0;
	virtual int Build(const char *module) = 0;
	virtual int Discard(const char *module) = 0;
	virtual int GetFunctionIDByName(const char *module, const char *name) = 0;
	virtual int GetFunctionIDByDecl(const char *module, const char *decl) = 0;
	virtual int GetFunctionIDByIndex(const char *module, int idx) = 0;
	virtual int GetFunctionCount(const char *module) = 0;
	virtual const char *GetFunctionName(int id, int *len = 0) = 0;
	virtual const char *GetFunctionDeclaration(int id, int *len = 0) = 0;
	virtual const char *GetFunctionModule(int id, int *len = 0) = 0;
	virtual int SetEngineProperty(asDWORD prop, asPWORD value) = 0;
	virtual const char *GetFunctionSection(int id, int *len = 0) = 0;
};
#define ANGELSCRIPT_VERSION 22200
#define ANGELSCRIPT_H
asIScriptEngine *asCreateScriptEngine(asDWORD version);
const char *asGetLibraryVersion();
const char *asGetLibraryOptions();
asIScriptContext *asGetActiveContext();
template<class T> asSFuncPtr asFunctionPtr(T f) { asSFuncPtr p; p.ptr = (void*)f; p.flag = 2; return p; }
template<class T> asSFuncPtr asSMethodPtr_(T f) { asSFuncPtr p; p.ptr = 0; p.flag = 3; return p; }
#define asFUNCTION(f) asFunctionPtr(f)
#define asFUNCTIONPR(f,p,r) asFunctionPtr((void (*)())((r (*)p)(f)))
#define asMETHOD(c,m) asSMethodPtr_(&c::m)
#define asMETHODPR(c,m,p,r) asSMethodPtr_((r (c::*)p)(&c::m))
#define asOFFSET(s,m) ((int)(size_t)(&reinterpret_cast<s*>(100000)->m)-100000)
