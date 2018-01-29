#include "MDFrameWork.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/Module.h>
#include <llvm/IR/Constants.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>
#include <llvm/ADT/StringSwitch.h>
#include "common/LLVMWarningsPop.hpp"

#include <iostream>
#include <assert.h>

using namespace llvm;

IGC::ModuleMetaData::ModuleMetaData()
{
    pushInfo.simplePushInfoArr.resize(g_c_maxNumberOfBufferPushed);
    m_ShaderResourceViewMcsMask.resize(NUM_SHADER_RESOURCE_VIEW_SIZE, 0);
}

//(non-autogen)function prototypes
MDNode* CreateNode(unsigned char i, Module* module, StringRef name);
MDNode* CreateNode(int i, Module* module, StringRef name);
MDNode* CreateNode(unsigned i, Module* module, StringRef name);
MDNode* CreateNode(uint64_t i, Module* module, StringRef name);
MDNode* CreateNode(float f, Module* module, StringRef name);
MDNode* CreateNode(bool b, Module* module, StringRef name);
template<typename val>
MDNode* CreateNode(const std::vector<val> &vec, Module* module, StringRef name);
MDNode* CreateNode(Value* val, Module* module, StringRef name);
template<typename Key, typename Value>
MDNode* CreateNode(const std::map < Key, Value> &FuncMD, Module* module, StringRef name);
MDNode* CreateNode(const std::string &s, Module* module, StringRef name);
MDNode* CreateNode(char* i, Module* module, StringRef name);

void readNode(bool &b, MDNode* node);
void readNode(float &x, MDNode* node);
void readNode(int &x, MDNode* node);
void readNode(uint64_t &x, MDNode* node);
void readNode(unsigned char &x, MDNode* node);
void readNode(Value* &val, MDNode* node);
void readNode(std::string &s, MDNode* node);
void readNode(char* &s, MDNode* node);

template<typename T>
void readNode(std::vector<T> &vec, MDNode* node);
void readNode(Function* &funcPtr, MDNode* node);
void readNode(GlobalVariable* &globalVar, MDNode* node);

template<typename Key, typename Value>
void readNode(std::map<Key, Value> &funcMD, MDNode* node);

template<typename T>
void readNode(T &t, MDNode* node, StringRef name);

//including auto-generated functions
#include "MDNodeFunctions.gen"
namespace IGC 
{
    bool operator < (const ConstantAddress &a, const ConstantAddress &b)
	{
		if (a.bufId < b.bufId)
			return true;
		else if (a.bufId == b.bufId)
			return (a.eltId < b.eltId);

		return false;
	}
}

// non-autogen functions implementations below
MDNode* CreateNode(unsigned char i, Module* module, StringRef name)
{
    Metadata* v[] =
    {
        MDString::get(module->getContext(), name),
        ValueAsMetadata::get(ConstantInt::get(Type::getInt8Ty(module->getContext()), i)),
    };
    MDNode* node = MDNode::get(module->getContext(), v);
    return node;
}

MDNode* CreateNode(int i, Module* module, StringRef name)
{
    Metadata* v[] =
    {
        MDString::get(module->getContext(), name),
        ValueAsMetadata::get(ConstantInt::get(Type::getInt32Ty(module->getContext()), i)),
    };
    MDNode* node = MDNode::get(module->getContext(), v);
    return node;
}

MDNode* CreateNode(unsigned i, Module* module, StringRef name)
{
    Metadata* v[] =
    {
        MDString::get(module->getContext(), name),
        ValueAsMetadata::get(ConstantInt::get(Type::getInt32Ty(module->getContext()), i)),
    };
    MDNode* node = MDNode::get(module->getContext(), v);
    return node;
}

MDNode* CreateNode(uint64_t i, Module* module, StringRef name)
{
	Metadata* v[] =
	{
		MDString::get(module->getContext(), name),
		ValueAsMetadata::get(ConstantInt::get(Type::getInt64Ty(module->getContext()), i)),
	};
	MDNode* node = MDNode::get(module->getContext(), v);
	return node;
}

MDNode* CreateNode(const std::string &s, Module* module, StringRef name)
{
    Metadata* v[] =
    {
        MDString::get(module->getContext(), name),
        MDString::get(module->getContext(), s),
    };
    MDNode* node = MDNode::get(module->getContext(), v);
    return node;
}

MDNode* CreateNode(float f, Module* module, StringRef name)
{
    Metadata* v[] =
    {
        MDString::get(module->getContext(), name),
        ValueAsMetadata::get(ConstantFP::get(Type::getFloatTy(module->getContext()), f)),
    };
   
    MDNode* node = MDNode::get(module->getContext(), v);
    return node;
}

MDNode* CreateNode(bool b, Module* module, StringRef name)
{
    Metadata* v[] =
    {
        MDString::get(module->getContext(), name),
        ValueAsMetadata::get(ConstantInt::get(Type::getInt1Ty(module->getContext()), b ? 1 : 0))
    };

    MDNode* node = MDNode::get(module->getContext(), v);
    return node;
}

template<typename val>
MDNode* CreateNode(const std::vector<val> &vec, Module* module, StringRef name)
{
    std::vector<Metadata*> nodes;
    nodes.push_back(MDString::get(module->getContext(), name));
    int i = 0;
    for (auto it = vec.begin(); it != vec.end(); ++it)
    {       
        nodes.push_back(CreateNode(*(it), module, name.str() + "Vec[" + std::to_string(i++) + "]"));
    }
    MDNode* node = MDNode::get(module->getContext(), nodes);
    return node;
}

MDNode* CreateNode(Value* val, Module* module, StringRef name)
{
    Metadata* v[] =
    {
        MDString::get(module->getContext(), name),
        ValueAsMetadata::get(val)
    };
    MDNode* node = MDNode::get(module->getContext(), v);
    return node;
}

template<typename Key, typename Value>
MDNode* CreateNode(const std::map < Key, Value> &FuncMD, Module* module, StringRef name)
{
    std::vector<Metadata*> nodes;
    nodes.push_back(MDString::get(module->getContext(), name));
    int i = 0;
    for ( auto it = FuncMD.begin(); it != FuncMD.end(); ++it)
    {
        nodes.push_back(CreateNode(it->first, module, name.str() + "Map[" + std::to_string(i) + "]"));
        nodes.push_back(CreateNode(it->second, module, name.str() + "Value[" +std::to_string(i++) + "]"));
    }
    MDNode* node = MDNode::get(module->getContext(), nodes);
    return node;
}

void readNode(unsigned char &b, MDNode* node)
{
    ValueAsMetadata* pVal = cast<ValueAsMetadata>(node->getOperand(1));
    b = ((unsigned char)cast<ConstantInt>(pVal->getValue())->getZExtValue()) ? true : false;
    return;
}

void readNode(char &b, MDNode* node)
{
	ValueAsMetadata* pVal = cast<ValueAsMetadata>(node->getOperand(1));
	b = ((char)cast<ConstantInt>(pVal->getValue())->getZExtValue()) ? true : false;
	return;
}


void readNode(bool &b, MDNode* node)
{   
    ValueAsMetadata* pVal = cast<ValueAsMetadata>(node->getOperand(1));
    b = ((int)cast<ConstantInt>(pVal->getValue())->getZExtValue()) ? true : false;
    return;  
}

void readNode(float &x, MDNode* node)
{    
    ValueAsMetadata* pVal = cast<ValueAsMetadata>(node->getOperand(1));
    x = (float)cast<ConstantFP>(pVal->getValue())->getValueAPF().convertToFloat();
    return;       
}

void readNode(int &x, MDNode* node)
{    
    ValueAsMetadata* pVal = cast<ValueAsMetadata>(node->getOperand(1));
    x = (int)cast<ConstantInt>(pVal->getValue())->getZExtValue();
    return;
}

void readNode(std::string &s, MDNode* node)
{
    ValueAsMetadata* pVal = cast<ValueAsMetadata>(node->getOperand(1));
    s = cast<llvm::MDString>(pVal)->getString();
    return;
}

void readNode(unsigned &x, MDNode* node)
{    
    ValueAsMetadata* pVal = cast<ValueAsMetadata>(node->getOperand(1));
    x = (unsigned)cast<ConstantInt>(pVal->getValue())->getZExtValue();
    return;
}

void readNode(uint64_t &x, MDNode* node)
{
	ValueAsMetadata* pVal = cast<ValueAsMetadata>(node->getOperand(1));
	x = (uint64_t)cast<ConstantInt>(pVal->getValue())->getZExtValue();
	return;
}

void readNode(Value* &val, MDNode* node)
{
    val = MetadataAsValue::get(node->getContext(), node->getOperand(1));
}

template<typename T>
void readNode(std::vector<T> &vec, MDNode* node)
{    
    for (unsigned k = 1; k < node->getNumOperands(); k++)
    {
        T vecEle;
        readNode(vecEle, cast<MDNode>(node->getOperand(k)));
        vec.push_back(vecEle);
    }
    return;
}

void readNode(Function* &funcPtr, MDNode* node)
{   
    ValueAsMetadata* pVal = cast<ValueAsMetadata>(node->getOperand(1));
    Value* v = pVal->getValue();
    funcPtr = cast<Function>(v);
}

void readNode(GlobalVariable* &globalVar, MDNode* node)
{
    ValueAsMetadata* pVal = cast<ValueAsMetadata>(node->getOperand(1));
    Value* v = pVal->getValue();
    globalVar = cast<GlobalVariable>(v);
}

template<typename Key, typename Value>
void readNode(std::map<Key, Value> &keyMD, MDNode* node)
{
    for (unsigned k = 1; k < node->getNumOperands(); k++)
    {
        std::pair<Key, Value> p;
        readNode(p.first, cast<MDNode>(node->getOperand(k++)));
        readNode(p.second, cast<MDNode>(node->getOperand(k)));
        keyMD.insert(p);
    }
    return;
}

template<typename T>
void readNode(T &t, MDNode* node, StringRef name)
{    
    for (unsigned i = 1; i < node->getNumOperands(); i++)
    {
        MDNode* temp = cast<MDNode>(node->getOperand(i));
        if (cast<MDString>(temp->getOperand(0))->getString() == name)
        {
            readNode(t, temp);
            return;
        }
    }
}

void IGC::deserialize(IGC::ModuleMetaData &deserializeMD, const Module* module)
{
	IGC::ModuleMetaData temp;
	deserializeMD = temp;
    NamedMDNode* root = module->getNamedMetadata("IGCMetadata");
    if (!root) { return; } //module has not been serialized with IGCMetadata yet
    MDNode* moduleRoot = root->getOperand(0);
    readNode(deserializeMD, moduleRoot);
}

void IGC::serialize(const IGC::ModuleMetaData &moduleMD, Module* module)
{
    NamedMDNode* LLVMMetadata = module->getNamedMetadata("IGCMetadata");
    if(LLVMMetadata)
    {
        // clear old metadata if present
        LLVMMetadata->dropAllReferences();
    }
    LLVMMetadata = module->getOrInsertNamedMetadata("IGCMetadata");
    auto node = CreateNode(moduleMD, module, "ModuleMD");
    LLVMMetadata->addOperand(node);
}
