/*
 * The contents of this file are subject to the Mozilla Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/MPL/
 * 
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 * 
 * The Original Code is TransforMiiX XSLT processor.
 * 
 * The Initial Developer of the Original Code is The MITRE Corporation.
 * Portions created by MITRE are Copyright (C) 1999 The MITRE Corporation.
 *
 * Portions created by Keith Visco as a Non MITRE employee,
 * (C) 1999 Keith Visco. All Rights Reserved.
 * 
 * Contributor(s): 
 * Keith Visco, kvisco@ziplink.net
 *    -- original author.
 *
 * $Id: NamedMap.h,v 1.6 2001/04/08 14:36:55 peterv%netscape.com Exp $
 */

/**
 * A Named Map for TxObjects
 * @author <a href="mailto:kvisco@ziplink.net">Keith Visco</a>
 * @version $Revision: 1.6 $ $Date: 2001/04/08 14:36:55 $
**/

#ifndef TRANSFRMX_NAMEDMAP_H
#define TRANSFRMX_NAMEDMAP_H

#include "baseutils.h"
#include "TxObject.h"
#include "StringList.h"

class NamedMap : public TxObject {


public:

      //----------------/
     //- Constructors -/
    //----------------/

    /**
     * Creates a new NodeStack with the default Size
    **/
    NamedMap();

    /**
     * Creates a new NodeStack with the specified number of buckets
    **/
    NamedMap(int size);

    /**
     * Destructor for a NamedMap table, will not delete references unless
     * The setObjectDeletion flag has been set to MB_TRUE
    **/
    virtual ~NamedMap();


    /**
     * Returns a list of all the keys of this NamedMap.
     * <BR />
     * You will need to delete this List when you are done with it.
    **/
    StringList* keys();

    /**
     *  Returns the object reference in this Map associated with the given name
     * @return the object reference in this Map associated with the given name
    **/
    TxObject* get(const String& name);

    /**
     *  Returns the object reference in this Map associated with the given name
     * @return the object reference in this Map associated with the given name
    **/
    TxObject* get(const char* name);

    /**
     *  Adds the Object reference to the map and associates it with the given name
    **/
    void  put(const String& name, TxObject* obj);

    /**
     *  Adds the Object reference to the map and associates it with the given name
    **/
    void  put(const char* name, TxObject* obj);

    /**
     * Removes all elements from the Map table
    **/
    void clear();

    void clear(MBool doObjectDeletion);

    /**
     * Returns true if the specified Node is contained in the set.
     * if the specfied Node is null, then if the NodeSet contains a null
     * value, true will be returned.
     * @param node the element to search the NodeSet for
     * @return true if specified Node is contained in the NodeSet
    **/
    //MBool contains(Node* node);

    /**
     * Compares the specified object with this NamedMap for equality.
     * Returns true if and only if the specified Object is a NamedMap
     * that hashes to the same value as this NamedMap
     * @return true if and only if the specified Object is a NamedMap
     * that hashes to the same value as this NamedMap
    **/
    MBool equals(NamedMap* namedMap);

    /**
     * Returns true if there are no Nodes in the NodeSet.
     * @return true if there are no Nodes in the NodeSet.
    **/
    MBool isEmpty();

    /**
     * Removes the Node at the specified index from the NodeSet
     * @param index the position in the NodeSet to remove the Node from
     * @return the Node that was removed from the list
    **/
    TxObject* remove(String& key);

    /**
     * Sets the object deletion flag. If set to true, objects in
     * the NamedMap will be deleted upon calling the clear() method, or
     * upon destruction. By default this is false.
    **/
    void  setObjectDeletion(MBool deleteObjects);

    /**
     * Returns the number of key-element pairs in the NamedMap
     * @return the number of key-element in the NamedMap
    **/
    int size();

    void dumpMap();




      //-------------------/
     //- Private Members -/
    //-------------------/


private:

    struct BucketItem {
        String key;
        TxObject* item;
        BucketItem* next;
        BucketItem* prev;
    };

    static const int DEFAULT_SIZE;

    // map table
    BucketItem** elements;

    Int32 numberOfBuckets;
    Int32 numberOfElements;
    MBool doObjectDeletion;

      //-------------------/
     //- Private Methods -/
    //-------------------/

    BucketItem* createBucketItem(const String& key, TxObject* objPtr);

    BucketItem* getBucketItem(const String& key);

    unsigned long hashKey(const String& key);

    /**
     * Helper method for constructors
    **/
    void initialize(int size);

}; //-- NamedMap

#endif

