/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/
#pragma once

#include "MetaDataTraits.h"
#include "MetaDataValue.h"
#include "MetaDataObject.h"
#include "MetaDataIterator.h"
#include "MapList.h"
#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/Value.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/LLVMContext.h>
#include "common/LLVMWarningsPop.hpp"
#include <vector>

namespace IGC
{

///
// Metadata list. It is assumed that
// all the nodes are of the same type ( as specified by the T template parameter)
// Template parameters:
// T - type of the entry node
// Traits - convertor type (see the MDValueTraits )
//
template<class T, class Traits = MDValueTraits<T> >
class MetaDataList: public IMetaDataObject
{
public:
    typedef IMetaDataObject _Mybase;
    typedef MetaDataList<T,Traits> _Myt;
    typedef MetaDataIterator<T, llvm::MDNode, Traits> meta_iterator;
    typedef typename Traits::value_type item_type;
    typedef typename std::vector<item_type>::iterator iterator;
    typedef typename std::vector<item_type>::const_iterator const_iterator;

    MetaDataList(const llvm::MDNode* pNode, bool hasId = false):
        _Mybase(pNode, hasId),
        m_pNode(pNode),
        m_isDirty(false),
        m_isLoaded(false)
    {}

    MetaDataList(const char* name):
        _Mybase(name),
        m_pNode(NULL),
        m_isDirty(false),
        m_isLoaded(false)
    {}

    MetaDataList():
        m_pNode(NULL),
        m_isDirty(false),
        m_isLoaded(false)
    {}

    void clear()
    {
        lazyLoad();
        m_data.clear();
        m_isDirty = true;
    }

    size_t size() const
    {
        lazyLoad();
        return m_data.size();
    }

    bool empty() const
    {
        lazyLoad();
        return m_data.empty();
    }

    iterator begin()
    {
        lazyLoad();
        return m_data.begin();
    }

    iterator end()
    {
        lazyLoad();
        return m_data.end();
    }

    const_iterator begin() const
    {
        lazyLoad();
        return m_data.begin();
    }

    const_iterator end() const
    {
        return m_data.end();
    }

    void push_back(const item_type& val)
    {
        lazyLoad();
        m_data.push_back(val);
        m_isDirty = true;
    }

    iterator erase(iterator where)
    {
        lazyLoad();
        iterator i = m_data.erase(where);
        m_isDirty = true;
        return i;
    }

    item_type getItem(size_t index) const
    {
        lazyLoad();
        return m_data[index];
    }

    void setItem( size_t index, const item_type& item)
    {
        lazyLoad();
        m_data[index] = item;
        m_isDirty = true;
    }

    bool dirty() const
    {
        if( m_isDirty )
        {
            return true;
        }

        if( !m_isLoaded )
        {
            return false;
        }

        for( const_iterator i = m_data.begin(), e = m_data.end(); i != e; ++i )
        {
            if( Traits::dirty(*i) )
            {
                return true;
            }
        }
        return false;
    }

    bool hasValue() const
    {
        return m_pNode != NULL || !empty() || dirty();
    }

    void discardChanges()
    {
        if( !dirty() )
        {
            return;
        }

        for( iterator i = m_data.begin(), e = m_data.end(); i != e; ++i )
        {
            Traits::discardChanges(*i);
        }

        m_isDirty = false;
    }

    virtual void save(llvm::LLVMContext &context, llvm::MDNode* pNode) const
    {
        if( m_pNode == pNode && !dirty() )
        {
            return;
        }

        if(pNode->getNumOperands() != size() + _Mybase::getStartIndex() )
        {
#if 0
            pNode->replaceAllUsesWith(generateNode(context));
            llvm::MDNode::deleteTemporary(pNode);
#else
            // todo llvm 3.6.0 transition
            assert(false);
#endif
            return;
        }

        _Mybase::save(context, pNode);

        meta_iterator mi(pNode, _Mybase::getStartIndex());
        meta_iterator me(pNode);
        const_iterator i = begin();
        const_iterator e = end();

        for(; i != e; ++i, ++mi )
        {
            Traits::save(context, *mi, *i);
        }
    }

    virtual llvm::Metadata* generateNode(llvm::LLVMContext &context) const
    {
        lazyLoad();

        llvm::SmallVector< llvm::Metadata*, 5> args;

        llvm::Metadata* pIDNode = _Mybase::generateNode(context);

        if( NULL != pIDNode )
        {
            args.push_back( pIDNode );
        }

        for( const_iterator i = m_data.begin(), e = m_data.end(); i != e; ++i )
        {
            args.push_back( Traits::generateValue(context, *i));
        }

        return llvm::MDNode::get(context,args);
    }

protected:

    virtual void lazyLoad() const
    {
        if( m_isLoaded || NULL == m_pNode )
        {
            return;
        }

        for(meta_iterator i(m_pNode, _Mybase::getStartIndex()), e(m_pNode); i != e; ++i )
        {
            m_data.push_back(i.get());
        }

        m_isLoaded = true;
    }


protected:
    const llvm::MDNode* m_pNode;
    bool m_isDirty;
    mutable bool m_isLoaded;
    mutable std::vector<item_type> m_data;
};

template<class T, class Traits = MDValueTraits<T> >
class NamedMDNodeList
{
public:
    typedef NamedMDNodeList<T,Traits> _Myt;
    typedef MetaDataIterator<T,llvm::NamedMDNode,Traits> meta_iterator;
    typedef typename Traits::value_type item_type;
    typedef typename std::vector<item_type>::iterator iterator;
    typedef typename std::vector<item_type>::const_iterator const_iterator;

    NamedMDNodeList(const llvm::NamedMDNode* pNode):
        m_pNode(pNode),
        m_isDirty(false),
        m_isLoaded(false)
    {}

    NamedMDNodeList():
        m_pNode(NULL),
        m_isDirty(false),
        m_isLoaded(true)
    {}

    void clear()
    {
        lazyLoad();
        m_data.clear();
        m_isDirty = true;
    }

    size_t size() const
    {
        lazyLoad();
        return m_data.size();
    }

    bool empty() const
    {
        lazyLoad();
        return m_data.empty();
    }

    iterator begin()
    {
        lazyLoad();
        return m_data.begin();
    }

    iterator end()
    {
        return m_data.end();
    }

    const_iterator begin() const
    {
        lazyLoad();
        return m_data.begin();
    }

    const_iterator end() const
    {
        return m_data.end();
    }

    void push_back(const item_type& val)
    {
        lazyLoad();
        m_data.push_back(val);
        m_isDirty = true;
    }

    item_type getItem( size_t index) const
    {
        lazyLoad();
        return m_data[index];
    }

    void setItem( size_t index, const item_type& item)
    {
        lazyLoad();
        m_data[index] = item;
        m_isDirty = true;
    }

    iterator erase(iterator where)
    {
        lazyLoad();
        iterator i = m_data.erase(where);
        m_isDirty = true;
        return i;
    }

    void save(llvm::LLVMContext &context, llvm::NamedMDNode* pNode) const
    {
        if(!dirty())
            return;

        assert(m_isLoaded && "Collection should be loaded at this point (since it is dirty)");

        if(pNode->getNumOperands() > size() )
            pNode->dropAllReferences();

        meta_iterator mi(pNode,0);
        meta_iterator me(pNode);
        const_iterator i = begin();
        const_iterator e = end();

        while( i != e || mi != me )
        {
            if( i != e && mi != me )
            {
                Traits::save(context, *mi, *i);
                ++i;
                ++mi;
            }
            else
            {
                assert( i != e && mi == me );
                pNode->addOperand(llvm::cast<llvm::MDNode>(Traits::generateValue(context, *i)));
                ++i;
            }
        }
    }

    bool dirty() const
    {
        if( m_isDirty )
        {
            return true;
        }

        if( !m_isLoaded )
        {
            return false;
        }

        for( const_iterator i = m_data.begin(), e = m_data.end(); i != e; ++i )
        {
            if( Traits::dirty(*i) )
                return true;
        }
        return false;
    }

    bool hasValue() const
    {
        return m_pNode != NULL || dirty();
    }

    void discardChanges()
    {
        if( !dirty() )
        {
            return;
        }

        for( iterator i = m_data.begin(), e = m_data.end(); i != e; ++i )
        {
            Traits::discardChanges(*i);
        }

        m_isDirty = false;
    }
private:

    void lazyLoad() const
    {
        if( m_isLoaded || NULL == m_pNode )
        {
            return;
        }

        for(meta_iterator i(m_pNode,0), e(m_pNode); i != e; ++i )
        {
            m_data.push_back(i.get());
        }

        m_isLoaded = true;
    }

private:
    const llvm::NamedMDNode* m_pNode;
    mutable std::vector<item_type> m_data;
    bool m_isDirty;
    mutable bool m_isLoaded;
};

template<class K, class T,
        class KeyTraits = MDValueTraits<K>,
        class ValTraits = MDValueTraits<T> >
class NamedMetaDataMap: public IMetaDataObject
{
public:
    typedef NamedMetaDataMap<K, T, KeyTraits, ValTraits> _Myt;
    typedef MetaDataIterator<llvm::MDNode, llvm::NamedMDNode, MDValueTraits<llvm::MDNode> > meta_iterator;
    typedef typename KeyTraits::value_type key_type;
    typedef typename ValTraits::value_type item_type;
    typedef IGC::MapList<key_type, item_type> MapImplType;
    typedef typename MapImplType::iterator iterator;
    typedef typename MapImplType::const_iterator const_iterator;

    NamedMetaDataMap(const llvm::NamedMDNode* pNode):
        m_pNode(pNode),
        m_isDirty(false),
        m_isLoaded(false)
    {}

    NamedMetaDataMap():
        m_pNode(NULL),
        m_isDirty(false),
        m_isLoaded(true)
    {}

    void clear()
    {
        lazyLoad();
        m_data.clear();
        m_isDirty = true;
    }

    size_t size() const
    {
        lazyLoad();
        return m_data.size();
    }

    bool empty() const
    {
        lazyLoad();
        return m_data.empty();
    }

    iterator begin()
    {
        lazyLoad();
        return m_data.begin();
    }

    iterator end()
    {
        return m_data.end();
    }

    const_iterator begin() const
    {
        lazyLoad();
        return m_data.begin();
    }

    const_iterator end() const
    {
        return m_data.end();
    }

    item_type getItem(const key_type& key) const
    {
        lazyLoad();
        if (find(key) == end())
        {
            std::string Msg = "Invalid user defined function being processed: ";
            Msg += key->getName();
            Msg += "()\n";
            assert(0 && "Trying to get key that does not exists in Metadata map");
            m_pNode->getParent()->getContext().emitError(Msg);
        }
        return m_data[key];
    }

    item_type getOrInsertItem(const key_type& key)
    {
        lazyLoad();
        if(find(key) == end())
        {
            m_data[key] = ValTraits::load(NULL);
            m_isDirty = true;
        }
        return m_data[key];
    }

    void setItem(const key_type& key, const item_type& item)
    {
        lazyLoad();
        m_data[key] = item;
        m_isDirty = true;
    }

    NamedMetaDataMap& operator=(const llvm::NamedMDNode* pNode) 
    {
        m_pNode = pNode;
        return (*this);
    }

    item_type& operator[]( const key_type& key )
    {
        lazyLoad();
        return &m_data[key];
    }

    const item_type& operator[]( const key_type& key ) const
    {
        lazyLoad();
        return &m_data[key];
    }

    iterator find(const key_type& key) const
    {
        lazyLoad();
        return m_data.find(key);
    }

    void erase(iterator where)
    {
        lazyLoad();
        m_data.erase(where);
        m_isDirty = true;
    }

    void save(llvm::LLVMContext &context, llvm::NamedMDNode* pNode) const
    {
        if(!dirty())
            return;

        assert(m_isLoaded && "Collection should be loaded at this point (since it is dirty)");

        pNode->dropAllReferences();

        meta_iterator mi(pNode,0);
        meta_iterator me(pNode);
        const_iterator i = begin();
        const_iterator e = end();

        while( i != e || mi != me )
        {
            assert( i != e && mi == me );
            llvm::SmallVector<llvm::Metadata*, 2> args;
            args.push_back(KeyTraits::generateValue(context, (*i).first));
            args.push_back(ValTraits::generateValue(context, (*i).second));
            pNode->addOperand(llvm::MDNode::get(context,args));
            ++i;
        }
    }

    bool dirty() const
    {
        if( m_isDirty )
        {
            return true;
        }

        if( !m_isLoaded )
        {
            return false;
        }

        for( const_iterator i = m_data.begin(), e = m_data.end(); i != e; ++i )
        {
            if( KeyTraits::dirty((*i).first) || ValTraits::dirty((*i).second) )
                return true;
        }
        return false;
    }

    bool hasValue() const
    {
        return m_pNode != NULL || dirty();
    }

    void discardChanges()
    {
        if( !dirty() )
        {
            return;
        }

        for( iterator i = m_data.begin(), e = m_data.end(); i != e; ++i )
        {
            KeyTraits::discardChanges((key_type&)((*i).first));
            ValTraits::discardChanges((item_type&)((*i).second));
        }

        m_isDirty = false;
    }
private:

    void lazyLoad() const
    {
        if( m_isLoaded || NULL == m_pNode )
        {
            return;
        }

        for(meta_iterator i(m_pNode,0), e(m_pNode); i != e; ++i )
        {
            llvm::MDNode *node = i.get();
            assert(node->getNumOperands() == 2 && "MetaDataMap node assumed to have exactly two operands");
            key_type key = KeyTraits::load(node->getOperand(0));
            item_type val = ValTraits::load(node->getOperand(1));
            m_data[key] = val;
        }

        m_isLoaded = true;
    }

private:
    const llvm::NamedMDNode* m_pNode;
    mutable MapImplType m_data;
    bool m_isDirty;
    mutable bool m_isLoaded;
};

class NamedMDFlag
{
public:
    NamedMDFlag(llvm::NamedMDNode* pNode):
        m_pNode(pNode),
        m_isDirty(false)
    {}

    NamedMDFlag():
        m_pNode(NULL),
        m_isDirty(false)
    {}

    void set() 
    {
        m_isDirty = m_pNode ? false : true;
    }

    void unSet()
    {
        m_isDirty = m_pNode ? true : false;
    }

    bool hasValue() const
    {
        return m_pNode != NULL || dirty();
    }

    bool dirty() const
    {
        return m_isDirty;
    }

    void discardChanges()
    {
        m_isDirty = false;
    }

    void save(llvm::LLVMContext &context, llvm::Module *module, const char* nodeName)
    {
        if(!dirty())
        {
            return;
        }
        if (!m_pNode)
        {
            m_pNode = module->getOrInsertNamedMetadata(nodeName);
        } 
        else 
        {
            module->eraseNamedMetadata(m_pNode);
            m_pNode = NULL;
        }
        discardChanges();
    }

private:
    llvm::NamedMDNode* m_pNode;
    bool m_isDirty;
};

} //namespace
