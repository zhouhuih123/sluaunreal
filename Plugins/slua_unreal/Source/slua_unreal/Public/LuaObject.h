// Tencent is pleased to support the open source community by making sluaunreal available.

// Copyright (C) 2018 THL A29 Limited, a Tencent company. All rights reserved.
// Licensed under the BSD 3-Clause License (the "License"); 
// you may not use this file except in compliance with the License. You may obtain a copy of the License at

// https://opensource.org/licenses/BSD-3-Clause

// Unless required by applicable law or agreed to in writing, 
// software distributed under the License is distributed on an "AS IS" BASIS, 
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
// See the License for the specific language governing permissions and limitations under the License.

#pragma once
#define LUA_LIB
#include "CoreMinimal.h"
#include "lua/lua.hpp"
#include "UObject/UnrealType.h"
#include "UObject/WeakObjectPtr.h"
#include "Blueprint/UserWidget.h"
#include "SluaUtil.h"
#include "LuaArray.h"
#include "LuaMap.h"
#include "Runtime/Launch/Resources/Version.h"

#ifndef SLUA_CPPINST
#define SLUA_CPPINST "__cppinst"
#endif 

// checkUD will report error if ud had been freed
#define CheckUD(Type,L,P) auto UD = LuaObject::checkUD<Type>(L,P);
// UD may be freed by Engine, so skip it in gc phase
#define CheckUDGC(Type,L,P) auto UD = LuaObject::checkUD<Type>(L,P,false); \
	if(!UD) return 0;

#define RegMetaMethodByName(L,NAME,METHOD) \
    lua_pushcfunction(L,METHOD); \
    lua_setfield(L,-2,NAME);

#define RegMetaMethod(L,METHOD) RegMetaMethodByName(L,#METHOD,METHOD)

#define NewUD(T, v, f) auto ud = lua_newuserdata(L, sizeof(UserData<T*>)); \
	if (!ud) luaL_error(L, "out of memory to new ud"); \
	auto udptr = reinterpret_cast< UserData<T*>* >(ud); \
	udptr->parent = nullptr; \
	udptr->ud = const_cast<T*>(v); \
    udptr->flag = f;

#define CheckSelf(T) \
	auto udptr = reinterpret_cast<UserData<T*>*>(lua_touserdata(L, 1)); \
	if(!udptr) luaL_error(L, "self ptr missing"); \
	if (udptr->flag & UD_HADFREE) \
		luaL_error(L, "checkValue error, obj parent has been freed"); \
	auto self = udptr->ud

#define IsRealOutParam(propflag) ((propflag&CPF_OutParm) && !(propflag&CPF_ConstParm) && !(propflag&CPF_BlueprintReadOnly))

namespace slua {

    class LuaVar;

    struct AutoStack {
        AutoStack(lua_State* l) {
            this->L = l;
            this->top = lua_gettop(L);
        }
        ~AutoStack() {
            lua_settop(this->L,this->top);
        }
        lua_State* L;
        int top;
    };

    struct LuaStruct : public FGCObject {
        uint8* buf;
        uint32 size;
        UScriptStruct* uss;

        LuaStruct(uint8* buf,uint32 size,UScriptStruct* uss);
        ~LuaStruct();

		virtual void AddReferencedObjects(FReferenceCollector& Collector) override;

#if (ENGINE_MINOR_VERSION>=20) && (ENGINE_MAJOR_VERSION>=4)
		virtual FString GetReferencerName() const override
		{
			return "LuaStruct";
		}
#endif
    };

		
	#define UD_NOFLAG 0
	#define UD_AUTOGC 1 // flag userdata should call __gc and maintain by lua
	#define UD_HADFREE 1<<2 // flag userdata had been freed
	#define UD_SHAREDPTR 1<<3 // it's a TSharedptr in userdata instead of raw pointer
	#define UD_THREADSAFEPTR 1<<4 // it's a TSharedptr with thread-safe mode in userdata instead of raw pointer
	#define UD_UOBJECT 1<<5 // flag it's an UObject

	// Memory layout of GenericUserData and UserData should be same
	struct GenericUserData {
		void* ud;
		uint32 flag;
		void* parent;
	};

	template<class T>
	struct UserData {
		T ud; 
		uint32 flag; 
		void* parent; 
		static_assert(sizeof(T)==sizeof(void*),"Userdata type should size equal to sizeof(void*)"); 
	};

    DefTypeName(LuaArray);
    DefTypeName(LuaMap);

    template<typename T>
    struct LuaOwnedPtr {
        T* ptr;
        LuaOwnedPtr(T* p):ptr(p) {}
    };

	template<typename T,ESPMode mode>
	struct SharedPtrUD {
		TSharedRef<T, mode> ref;
		SharedPtrUD(const TSharedPtr<T, mode>& other) :ref(other.ToSharedRef()) {}
		SharedPtrUD(const TSharedRef<T, mode>& other) :ref(other) {}
	};

	FString getUObjName(UObject* obj);

    class SLUA_UNREAL_API LuaObject
    {
    private:

#define CHECK_UD_VALID(ptr) if (ptr && ptr->flag&UD_HADFREE) { \
		if (checkfree) \
			luaL_error(L, "arg %d had been freed(%p), can't be used", lua_absindex(L, p), ptr->ud); \
		else \
			return nullptr; \
		}

		template<typename T>
		static T* maybeAnUDTable(lua_State* L, int p,bool checkfree) {
			if(lua_istable(L, p)) {
                AutoStack as(L);
				lua_getfield(L, p, SLUA_CPPINST);
				if (lua_type(L, -1) == LUA_TUSERDATA)
					return checkUD<T>(L, lua_absindex(L, -1), checkfree);
			}
			return nullptr;
		}

        // testudata, if T is base of uobject but isn't uobject, try to  cast it to T
        template<typename T>
        static typename std::enable_if<std::is_base_of<UObject,T>::value && !std::is_same<UObject,T>::value, T*>::type testudata(lua_State* L,int p, bool checkfree=true) {
            UserData<UObject*>* ptr = (UserData<UObject*>*)luaL_testudata(L,p,"UObject");
			CHECK_UD_VALID(ptr);
            T* t = ptr?Cast<T>(ptr->ud):nullptr;
			if (!t && lua_isuserdata(L, p)) {
				luaL_getmetafield(L, p, "__name");
				if (lua_isnil(L, -1)) {
					lua_pop(L, 1);
					return t;
				}
				FString clsname(lua_tostring(L, -1));
				lua_pop(L, 1);
				// skip firat char may be 'U' or 'A'
				if (clsname.Find(T::StaticClass()->GetName()) == 1) {
					UserData<T*>* tptr = (UserData<T*>*) lua_touserdata(L, p);
					t = tptr ? tptr->ud : nullptr;
				}
			}
			else if (!t)
				return maybeAnUDTable<T>(L, p,checkfree);
            return t;
        }

        // testudata, if T is uobject
        template<typename T>
        static typename std::enable_if<std::is_same<UObject,T>::value, T*>::type testudata(lua_State* L,int p, bool checkfree=true) {
            auto ptr = (UserData<T*>*)luaL_testudata(L,p,"UObject");
			CHECK_UD_VALID(ptr);
			if (!ptr) return maybeAnUDTable<T>(L, p, checkfree);
            return ptr?ptr->ud:nullptr;
        }

        // testudata, if T isn't uobject
        template<typename T>
        static typename std::enable_if<!std::is_base_of<UObject,T>::value && !std::is_same<UObject,T>::value, T*>::type 
		testudata(lua_State* L,int p,bool checkfree=true) {
            auto ptr = (UserData<T*>*)luaL_testudata(L,p,TypeName<T>::value().c_str());
			CHECK_UD_VALID(ptr);
			// ptr is boxed shared ptr?
			if (ptr && ptr->flag&UD_SHAREDPTR) {
				// ptr may be a ThreadSafe sharedref or NotThreadSafe sharedref
				// but we don't care about it, we just Get raw ptr from it
				static_assert(sizeof(SharedPtrUD<T, ESPMode::NotThreadSafe>) == sizeof(SharedPtrUD<T, ESPMode::ThreadSafe>), "unexpected static assert");
				auto sptr = (UserData<SharedPtrUD<T, ESPMode::NotThreadSafe>*>*)ptr;
				// unbox shared ptr to rawptr
				return &sptr->ud->ref.Get();
			}
			if (!ptr) return maybeAnUDTable<T>(L, p, checkfree);
            return ptr?ptr->ud:nullptr;
        }

    public:

        typedef int (*PushPropertyFunction)(lua_State* L,UProperty* prop,uint8* parms);
        typedef int (*CheckPropertyFunction)(lua_State* L,UProperty* prop,uint8* parms,int i);

        static CheckPropertyFunction getChecker(UClass* prop);
        static PushPropertyFunction getPusher(UProperty* prop);
        static CheckPropertyFunction getChecker(UProperty* cls);
        static PushPropertyFunction getPusher(UClass* cls);

		static bool matchType(lua_State* L, int p, const char* tn);

		static int classIndex(lua_State* L);
		static int classNewindex(lua_State* L);

		static void newType(lua_State* L, const char* tn);
        static void newTypeWithBase(lua_State* L, const char* tn, std::initializer_list<const char*> bases);
		static void addMethod(lua_State* L, const char* name, lua_CFunction func, bool isInstance = true);
		static void addGlobalMethod(lua_State* L, const char* name, lua_CFunction func);
		static void addField(lua_State* L, const char* name, lua_CFunction getter, lua_CFunction setter, bool isInstance = true);
		static void addOperator(lua_State* L, const char* name, lua_CFunction func);
		static void finishType(lua_State* L, const char* tn, lua_CFunction ctor, lua_CFunction gc, lua_CFunction strHint=nullptr);
		// create new enum type to lua, see DefEnum macro
		template<class T>
		static void newEnum(lua_State* L, const char* tn, const char* keys, std::initializer_list<T> values) {
			FString fkey(keys);
			// remove namespace prefix
			fkey.ReplaceInline(*FString::Format(TEXT("{0}::"), { UTF8_TO_TCHAR(tn) }), TEXT(""));
			// remove space
			fkey.ReplaceInline(TEXT(" "),TEXT(""));
			// create enum table
			createTable(L, tn);

			FString key, right;
			for (size_t i = 0; i < values.size() && strSplit(fkey, ",", &key, &right); ++i) {
				lua_pushinteger(L, (int)(*(values.begin() + i)));
				lua_setfield(L, -2, TCHAR_TO_UTF8(*key));
				fkey = right;
			}
			// pop t
			lua_pop(L, 1);
		}
        static void init(lua_State* L);


        


        // check arg at p is exported lua class named __name in field 
        // of metable of the class, if T is base of class or class is T, 
        // return the pointer of class, otherwise return nullptr
        template<typename T>
        static T* checkUD(lua_State* L,int p,bool checkfree=true) {
			if (lua_isnil(L, p))
			{
				return nullptr;
			}

            T* ret = testudata<T>(L,p, checkfree);
            if(ret) return ret;

            const char *typearg = nullptr;
            if (luaL_getmetafield(L, p, "__name") == LUA_TSTRING)
                typearg = lua_tostring(L, -1);
                
            lua_pop(L,1);

            if(checkfree && !typearg)
                luaL_error(L,"expect userdata at %d",p);

			if (LuaObject::isBaseTypeOf(L, typearg, TypeName<T>::value().c_str())) {
				UserData<T*> *udptr = reinterpret_cast<UserData<T*>*>(lua_touserdata(L, p));
				CHECK_UD_VALID(udptr);
				return udptr->ud;
			}
            if(checkfree) 
				luaL_error(L,"expect userdata %s, but got %s",TypeName<T>::value().c_str(),typearg);
            return nullptr;
        }

		template<class T>
		static T checkValueOpt(lua_State* L, int p, const T& defaultValue=T()) {
			if (!typeMatched<T>(lua_type(L,p))) {
				return defaultValue;
			} else {
				return checkValue<T>(L, p);
			}
		}

		template<class T>
		static typename std::enable_if< std::is_pointer<T>::value,T >::type checkReturn(lua_State* L, int p) {
			void* ud = lua_touserdata(L, p);
			UserData<T> *udptr = reinterpret_cast<UserData<T>*>(ud);
			if (udptr->flag & UD_HADFREE)
				luaL_error(L, "checkValue error, obj parent has been freed");
			return udptr->ud;
		}

		// if T isn't pointer, we should assume UserData box a pointer and dereference it to return
		template<class T>
		static typename std::enable_if< !std::is_pointer<T>::value,T >::type checkReturn(lua_State* L, int p) {
			void* ud = lua_touserdata(L, p);
			UserData<T*> *udptr = reinterpret_cast<UserData<T*>*>(ud);
			if (udptr->flag & UD_HADFREE)
				luaL_error(L, "checkValue error, obj parent has been freed");
			return *(udptr->ud);
		}
        
        template<class T>
		static T checkValue(lua_State* L, int p) {
            if(std::is_pointer<T>::value && lua_isnil(L,p))
                return T();

            using TT = typename remove_cr<T>::type;
            if(std::is_class<TT>::value && std::is_default_constructible<TT>::value && lua_isnil(L,p))
                return TT();

			static_assert(!std::is_same<wchar_t*, typename remove_ptr_const<T>::type>::value,
				"checkValue does not support parameter const TCHAR*, use FString instead");

			if (!lua_isuserdata(L, p))
				luaL_error(L, "expect userdata at arg %d", p);

			return checkReturn<T>(L, p);
		}

		template<class T>
		bool checkValue(lua_State* L, int p, T& out) {
			if (!typeMatched<T>(lua_type(L, p)))
				return false;
			out = checkValue<T>(L, p);
			return true;
		}

		// check value if it's enum
		template<typename T>
		static T checkEnumValue(lua_State* L, int p) {
			return static_cast<T>(luaL_checkinteger(L, p));
		}

        // check value if it's TArray
        template<class T>
		static T checkTArray(lua_State* L, int p) {
            CheckUD(LuaArray,L,p);
			return UD->asTArray<typename T::ElementType>(L);
		}

		// check value if it's TMap
		template<class T>
		static T checkTMap(lua_State* L, int p) {
			CheckUD(LuaMap, L, p);
			using KeyType = typename TPairTraits<typename T::ElementType>::KeyType;
			using ValueType = typename TPairTraits<typename T::ElementType>::ValueType;
			return UD->asTMap<KeyType, ValueType>(L);
		}

        template<class T>
        static UObject* checkUObject(lua_State* L,int p) {
            UserData<UObject*>* ud = reinterpret_cast<UserData<UObject*>*>(luaL_checkudata(L, p,"UObject"));
            if(!ud) luaL_error(L, "checkValue error at %d",p);
            return Cast<T>(ud->ud);
        }

        template<typename T>
        static void* void_cast( const T* v ) {
            return reinterpret_cast<void *>(const_cast< T* >(v));
        }

        template<typename T>
        static void* void_cast( T* v ) {
            return reinterpret_cast<void *>(v);
        }

		template<class T>
		static int push(lua_State* L, const char* fn, const T* v, uint32 flag = UD_NOFLAG) {
            if(getFromCache(L,void_cast(v),fn)) return 1;
			NewUD(T, v, flag);
            luaL_getmetatable(L,fn);
			lua_setmetatable(L, -2);
            cacheObj(L,void_cast(v));
            return 1;
		}

		static void releaseLink(lua_State* L, void* prop);
		static void linkProp(lua_State* L, void* parent, void* prop);

		template<class T>
		static int pushAndLink(lua_State* L, const void* parent, const char* tn, const T* v) {
			if (getFromCache(L, void_cast(v), tn)) return 1;
			NewUD(T, v, UD_NOFLAG);
			luaL_getmetatable(L, tn);
			lua_setmetatable(L, -2);
			cacheObj(L, void_cast(v));
			linkProp(L, void_cast(parent), void_cast(udptr));
			return 1;
		}

        typedef void SetupMetaTableFunc(lua_State* L,const char* tn,lua_CFunction setupmt,lua_CFunction gc);

        template<class T, bool F = IsUObject<T>::value >
        static int pushType(lua_State* L,T cls,const char* tn,lua_CFunction setupmt=nullptr,lua_CFunction gc=nullptr) {
            if(!cls) {
                lua_pushnil(L);
                return 1;
            }
            UserData<T>* ud = reinterpret_cast< UserData<T>* >(lua_newuserdata(L, sizeof(UserData<T>)));
			ud->parent = nullptr;
            ud->ud = cls;
            ud->flag = gc!=nullptr?UD_AUTOGC:UD_NOFLAG;
			if (F) ud->flag |= UD_UOBJECT;
            setupMetaTable(L,tn,setupmt,gc);
            return 1;
        }

		// for TSharePtr version

		template<class T, ESPMode mode>
		static int gcSharedPtrUD(lua_State* L) {
			luaL_checktype(L, 1, LUA_TUSERDATA);
			using BOXPUD = SharedPtrUD<T, mode>;
			UserData<BOXPUD*>* ud = reinterpret_cast<UserData<BOXPUD*>*>(lua_touserdata(L, 1));
			ud->flag |= UD_HADFREE;
			SafeDelete(ud->ud);
			return 0;
		}

		template<class T,ESPMode mode, bool F = IsUObject<T>::value>
		static int pushType(lua_State* L, SharedPtrUD<T, mode>* cls, const char* tn) {
			if (!cls) {
				lua_pushnil(L);
				return 1;
			}
			using BOXPUD = SharedPtrUD<T, mode>;
			UserData<BOXPUD*>* ud = reinterpret_cast<UserData<BOXPUD*>*>(lua_newuserdata(L, sizeof(UserData<BOXPUD*>)));
			ud->parent = nullptr;
			ud->ud = cls;
			ud->flag = UD_AUTOGC|UD_SHAREDPTR;
			if (F) ud->flag |= UD_UOBJECT;
			if (mode == ESPMode::ThreadSafe) ud->flag |= UD_THREADSAFEPTR;
			setupMetaTable(L, tn, gcSharedPtrUD<T,mode>);
			return 1;
		}

		static void addRef(lua_State* L,UObject* obj, void* ud);
        static void removeRef(lua_State* L,UObject* obj);

        template<typename T>
        static int pushGCObject(lua_State* L,T obj,const char* tn,lua_CFunction setupmt,lua_CFunction gc) {
            if(getFromCache(L,obj,tn)) return 1;
            lua_pushcclosure(L,gc,0);
            int f = lua_gettop(L);
            int r = pushType<T>(L,obj,tn,setupmt,f);
            lua_remove(L,f); // remove wraped gc function
			if (r) {
				addRef(L, obj , lua_touserdata(L,-1));
				cacheObj(L, obj);
			}
            return r;
        }

        template<typename T>
        static int pushObject(lua_State* L,T obj,const char* tn,lua_CFunction setupmt=nullptr) {
            if(getFromCache(L,obj,tn)) return 1;
            int r = pushType<T>(L,obj,tn,setupmt,nullptr);
            if(r) cacheObj(L,obj);
            return r;
        }

		static int setupMTSelfSearch(lua_State* L);
        
        static int pushClass(lua_State* L,UClass* cls);
        static int pushStruct(lua_State* L,UScriptStruct* cls);
		static int pushEnum(lua_State* L, UEnum* e);
		static int push(lua_State* L, UObject* obj);
		inline static int push(lua_State* L, const UObject* obj) {
			return push(L, const_cast<UObject*>(obj));
		}
		static int push(lua_State* L, FScriptDelegate* obj);
		static int push(lua_State* L, LuaStruct* ls);
		static int push(lua_State* L, double v);
        static int push(lua_State* L, int64 v);
        static int push(lua_State* L, uint64 v);
        static int push(lua_State* L, int8 v);
        static int push(lua_State* L, uint8 v);
        static int push(lua_State* L, int16 v);
        static int push(lua_State* L, uint16 v);
		static int push(lua_State* L, float v);
		static int push(lua_State* L, int v);
		static int push(lua_State* L, bool v);
		static int push(lua_State* L, uint32 v);
		static int push(lua_State* L, void* v);
		static int push(lua_State* L, const FText& v);
		static int push(lua_State* L, const FString& str);
		static int push(lua_State* L, const FName& str);
		static int push(lua_State* L, const char* str);
		static int push(lua_State* L, const LuaVar& v);
        static int push(lua_State* L, UFunction* func, UClass* cls=nullptr);
		static int push(lua_State* L, UProperty* up, uint8* parms);
		static int push(lua_State* L, UProperty* up, UObject* obj);
		static int push(lua_State* L, const LuaLString& lstr);

        // check tn is base of base
        static bool isBaseTypeOf(lua_State* L,const char* tn,const char* base);

        template<typename T>
        static int push(lua_State* L,T* ptr,typename std::enable_if<!std::is_base_of<UObject,T>::value>::type* = nullptr) {
            return push(L,TypeName<T>::value().c_str(),ptr);
        }

        template<typename T>
        static int push(lua_State* L,LuaOwnedPtr<T> ptr) {
            return push(L,TypeName<T>::value().c_str(),ptr.ptr,UD_AUTOGC);
        }

		static int gcSharedPtr(lua_State *L) {
			return 0;
		}

		template<typename T, ESPMode mode>
		static int push(lua_State* L, const TSharedPtr<T, mode>& ptr) {
			// get raw ptr from sharedptr
			T* rawptr = ptr.Get();
			// get typename 
			auto tn = TypeName<T>::value();
			if (getFromCache(L, rawptr, tn.c_str())) return 1;
			int r = pushType<T>(L, new SharedPtrUD<T, mode>(ptr), tn.c_str());
			if (r) cacheObj(L, rawptr);
			return r;
		}

		template<typename T, ESPMode mode>
		static int push(lua_State* L, const TSharedRef<T, mode>& ref) {
			// get raw ptr from sharedptr
			T& rawref = ref.Get();
			// get typename 
			auto tn = TypeName<T>::value();
			if (getFromCache(L, &rawref, tn.c_str())) return 1;
			int r = pushType<T>(L, new SharedPtrUD<T, mode>(ref), tn.c_str());
			if (r) cacheObj(L, &rawref);
			return r;
		}

		// for TBaseDelegate
		template<class R, class ...ARGS>
		static int push(lua_State* L, TBaseDelegate<R, ARGS...>& delegate);
		

        template<typename T>
        static int push(lua_State* L,T v,typename std::enable_if<std::is_enum<T>::value>::type* = nullptr) {
            return push(L,static_cast<int>(v));
        }

		template<typename T>
		static int push(lua_State* L, const TArray<T>& v) {
			return LuaArray::push(L, v);
		}

		template<typename K,typename V>
		static int push(lua_State* L, const TMap<K,V>& v) {
			return LuaMap::push(L, v);
		}

		// static int push(lua_State* L, FScriptArray* array);
        
        static int pushNil(lua_State* L) {
            lua_pushnil(L);
            return 1;
        }

        static void addExtensionMethod(UClass* cls,const char* n,lua_CFunction func,bool isStatic=false);

        static UFunction* findCacheFunction(lua_State* L,UClass* cls,const char* fname);
        static void cacheFunction(lua_State* L, UClass* cls,const char* fame,UFunction* func);

        static bool getFromCache(lua_State* L, void* obj, const char* tn, bool check = true);
		static void cacheObj(lua_State* L, void* obj);
		static void removeFromCache(lua_State* L, void* obj);
		static void deleteFGCObject(lua_State* L,FGCObject* obj);
    private:
        static int setupClassMT(lua_State* L);
        static int setupInstanceMT(lua_State* L);
        static int setupInstanceStructMT(lua_State* L);
        static int setupStructMT(lua_State* L);

        static int gcObject(lua_State* L);
        static int gcClass(lua_State* L);
        static int gcStructClass(lua_State* L);
		static int gcStruct(lua_State* L);
        static int objectToString(lua_State* L);
        static void setupMetaTable(lua_State* L,const char* tn,lua_CFunction setupmt,lua_CFunction gc);
		static void setupMetaTable(lua_State* L, const char* tn, lua_CFunction setupmt, int gc);
		static void setupMetaTable(lua_State* L, const char* tn, lua_CFunction gc);

        template<class T, bool F = IsUObject<T>::value>
        static int pushType(lua_State* L,T cls,const char* tn,lua_CFunction setupmt,int gc) {
            if(!cls) {
                lua_pushnil(L);
                return 1;
            }
                
            UserData<T>* ud = reinterpret_cast< UserData<T>* >(lua_newuserdata(L, sizeof(UserData<T>)));
			ud->parent = nullptr;
            ud->ud = cls;
            ud->flag = F|UD_AUTOGC;
			if (F) ud->flag |= UD_UOBJECT;
            setupMetaTable(L,tn,setupmt,gc);
            return 1;
        }
		static void createTable(lua_State* L, const char* tn);
    };

    template<>
    inline UClass* LuaObject::checkValue(lua_State* L, int p) {
        CheckUD(UClass, L, p);
        return UD;
    }

    template<>
    inline UObject* LuaObject::checkValue(lua_State* L, int p) {
		CheckUD(UObject, L, p);
		return UD;
    }

    template<>
    inline UScriptStruct* LuaObject::checkValue(lua_State* L, int p) {
        CheckUD(UScriptStruct, L, p);
        return UD;
    }

    template<>
    inline LuaStruct* LuaObject::checkValue(lua_State* L, int p) {
        CheckUD(LuaStruct, L, p);
        return UD;
    }

    template<>
    inline const char* LuaObject::checkValue(lua_State* L, int p) {
        return luaL_checkstring(L, p);
    }

    template<>
    inline float LuaObject::checkValue(lua_State* L, int p) {
        return (float)luaL_checknumber(L, p);
    }

    template<>
    inline double LuaObject::checkValue(lua_State* L, int p) {
        return luaL_checknumber(L, p);
    }

    template<>
    inline int LuaObject::checkValue(lua_State* L, int p) {
        return luaL_checkinteger(L, p);
    }

    template<>
    inline uint32 LuaObject::checkValue(lua_State* L, int p) {
        return (uint32) luaL_checkinteger(L, p);
    }

    template<>
    inline int8 LuaObject::checkValue(lua_State* L, int p) {
        return (int8) luaL_checkinteger(L, p);
    }

    template<>
    inline uint8 LuaObject::checkValue(lua_State* L, int p) {
        return (uint8) luaL_checkinteger(L, p);
    }

    template<>
    inline int16 LuaObject::checkValue(lua_State* L, int p) {
        return (int16) luaL_checkinteger(L, p);
    }

    template<>
    inline uint16 LuaObject::checkValue(lua_State* L, int p) {
        return (uint16) luaL_checkinteger(L, p);
    }

    template<>
    inline int64 LuaObject::checkValue(lua_State* L, int p) {
        return luaL_checkinteger(L, p);
    }

    template<>
    inline uint64 LuaObject::checkValue(lua_State* L, int p) {
        return luaL_checkinteger(L, p);
    }

    template<>
    inline bool LuaObject::checkValue(lua_State* L, int p) {
        luaL_checktype(L, p, LUA_TBOOLEAN);
        return !!lua_toboolean(L, p);
    }

    template<>
    inline FText LuaObject::checkValue(lua_State* L, int p) {
        const char* s = luaL_checkstring(L, p);
        return FText::FromString(UTF8_TO_TCHAR(s));
    }

    template<>
    inline FString LuaObject::checkValue(lua_State* L, int p) {
        const char* s = luaL_checkstring(L, p);
        return FString(UTF8_TO_TCHAR(s));
    }

    template<>
    inline FName LuaObject::checkValue(lua_State* L, int p) {
        const char* s = luaL_checkstring(L, p);
        return FName(UTF8_TO_TCHAR(s));
    }

    template<>
    inline void* LuaObject::checkValue(lua_State* L, int p) {
        luaL_checktype(L,p,LUA_TLIGHTUSERDATA);
        return lua_touserdata(L,p);
    }
}