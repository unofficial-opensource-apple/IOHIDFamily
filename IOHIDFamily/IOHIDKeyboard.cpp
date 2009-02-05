/*
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

/*
 * 13 Aug 2002 		ryepez
 *			This class is based off IOHIKeyboard and handles
 *			USB HID report based keyboard devices
 */

#include <IOKit/IOLib.h>
#include <IOKit/assert.h>
#include <IOKit/hidsystem/IOHIDUsageTables.h>
#include <IOKit/hidsystem/IOHIDParameter.h>
#include <IOKit/hidsystem/IOHIDShared.h>
#include <IOKit/usb/USB.h>

#include "IOHIDKeyboard.h"
#include "IOHIDKeys.h"
#include "IOHIDElement.h"

#define kFnModifierUsagePageKey		"FnModifierUsagePage"
#define kFnModifierUsageKey		"FnModifierUsage"
#define kFnSpecialKeyMapKey		"FnSpecialKeyMap"
#define	kFnNonSpecialUsageMapKey	"FnNonSpecialUsageMap"
#define	kNumPadUsageMapKey		"NumPadUsageMap"

#define super IOHIKeyboard

OSDefineMetaClassAndStructors(IOHIDKeyboard, IOHIKeyboard)

extern unsigned char hid_usb_2_adb_keymap[];  //In Cosmo_USB2ADB.cpp

IOHIDKeyboard * 
IOHIDKeyboard::Keyboard(OSArray *elements, IOHIDDevice *owner) 
{
    IOHIDKeyboard *keyboard = new IOHIDKeyboard;
    
    if ((keyboard == 0) || !keyboard->init() || 
            !keyboard->findDesiredElements(elements, owner))
    {
        if (keyboard) keyboard->release();
        return 0;
    }

    return keyboard;
}


bool
IOHIDKeyboard::init(OSDictionary *properties)
{
  if (!super::init(properties))  return false;
    
    _oldmodifier 	= 0;  
    _asyncLEDThread 	= 0;
    _ledState 		= 0;
    _fKeyMode 		= 0;
    _lastFKeyValue 	= 0;
    _numLeds 		= 0;
    _consumer 		= 0;
    _publishNotify 	= 0;
    _stickyKeysOn 	= false;

    _vendorID		= 0;
    _productID		= 0;
    _locationID		= 0;
    _transport		= 0;
    
    _keyboardLock = IORecursiveLockAlloc(); 
        
    _keyCodeArrayValuePtrArray = 0;
        
    _ledCookies[0] = -1;
    _ledCookies[1] = -1;
    
    bzero(_modifierValuePtrs, sizeof(UInt32*)*8);
    bzero(_ledValuePtrs, sizeof(UInt32*)*2);
    bzero(_secondaryKeys, sizeof(SecondaryKey)*255);
    _fKeyValuePtr = 0;
    
    //This makes separate copy of ADB translation table.  Needed to allow ISO
    //  keyboards to swap two keys without affecting non-ISO keys that are
    //  also connected, or that will be plugged in later through USB ports
    bcopy(hid_usb_2_adb_keymap, _usb_2_adb_keymap, ADB_CONVERTER_LEN);
      
    return true;
}


bool
IOHIDKeyboard::start(IOService *provider)
{
    OSNumber *xml_swap_CTRL_CAPSLOCK;
    OSNumber *xml_swap_CMD_ALT;
    UInt16 productIDVal;
    UInt16 vendorIDVal;

    _provider = provider;
    
    _transport	= OSDynamicCast(OSString,provider->getProperty(kIOHIDTransportKey));
    _vendorID	= OSDynamicCast(OSNumber,provider->getProperty(kIOHIDVendorIDKey));
    _productID	= OSDynamicCast(OSNumber,provider->getProperty(kIOHIDProductIDKey));
    _locationID	= OSDynamicCast(OSNumber,provider->getProperty(kIOHIDLocationIDKey));
    
    setProperty(kIOHIDTransportKey, _transport);
    setProperty(kIOHIDVendorIDKey, _vendorID);
    setProperty(kIOHIDProductIDKey, _productID);
    setProperty(kIOHIDLocationIDKey, _locationID);
    setProperty(kIOHIDCountryCodeKey, provider->getProperty(kIOHIDCountryCodeKey));

    productIDVal = _productID ? _productID->unsigned16BitValue() : 0;
    vendorIDVal = _vendorID ? _vendorID->unsigned16BitValue() : 0;

    if (!super::start(provider))
        return false;
    
    // Fix hardware bug in iMac USB keyboard mapping for ISO keyboards
    // This should really be done in personalities.
    if ( ((productIDVal == kprodUSBAndyISOKbd) || (productIDVal == kprodUSBCosmoISOKbd) || 
            (productIDVal == kprodQ6ISOKbd) || (productIDVal == kprodQ30ISOKbd))
            && (vendorIDVal == kIOUSBVendorIDAppleComputer))
    {
            _usb_2_adb_keymap[0x35] = 0x0a;  //Cosmo key18 swaps with key74, 0a is ADB keycode
            _usb_2_adb_keymap[0x64] = 0x32;
    }
        

    xml_swap_CTRL_CAPSLOCK = OSDynamicCast( OSNumber, provider->getProperty("Swap control and capslock"));
    if (xml_swap_CTRL_CAPSLOCK)
    {
	if ( xml_swap_CTRL_CAPSLOCK->unsigned32BitValue())
	{
	    char temp;
	    
	    temp = _usb_2_adb_keymap[0x39];  //Caps lock
            _usb_2_adb_keymap[0x39] = _usb_2_adb_keymap[0xe0];  //Left CONTROL modifier
            _usb_2_adb_keymap[0xe0] = temp;
	}
    }

    xml_swap_CMD_ALT = OSDynamicCast( OSNumber, provider->getProperty("Swap command and alt"));
    if (xml_swap_CMD_ALT)
    {
	if ( xml_swap_CMD_ALT->unsigned32BitValue())
	{
	    char temp;
	    
	    temp = _usb_2_adb_keymap[0xe2];  //left alt
            _usb_2_adb_keymap[0xe2] = _usb_2_adb_keymap[0xe3];  //Left command modifier
            _usb_2_adb_keymap[0xe3] = temp;
            
            temp = _usb_2_adb_keymap[0xe6];  //right alt
            _usb_2_adb_keymap[0xe6] = _usb_2_adb_keymap[0xe7];  //right command modifier
            _usb_2_adb_keymap[0xe7] = temp;

	}
    }

    // Need separate thread to handle LED
    _asyncLEDThread = thread_call_allocate((thread_call_func_t)_asyncLED, (thread_call_param_t)this);
    
    // Set up notification for the Consumer if there is a fnKey defined
    if ( _fKeyValuePtr )
    {
        OSDictionary *	matchingDictionary;
        
        matchingDictionary = IOService::serviceMatching( "IOHIDConsumer" );
        
        if( matchingDictionary )
        {
            matchingDictionary->setObject(kIOHIDTransportKey, _transport);
            matchingDictionary->setObject(kIOHIDVendorIDKey, _vendorID);
            matchingDictionary->setObject(kIOHIDProductIDKey, _productID);
            matchingDictionary->setObject(kIOHIDLocationIDKey, _locationID);
    
            _publishNotify = addNotification( gIOPublishNotification, 
                                matchingDictionary,
                                &IOHIDKeyboard::_publishNotificationHandler,
                                this, 0 );                                
        }
        
        findSecondaryKeys();
        setProperty(kIOHIDFKeyModeKey, _fKeyMode, sizeof(_fKeyMode));
    }
    
    return true;

}

bool IOHIDKeyboard::_publishNotificationHandler(
			void * target,
			void * /* ref */,
			IOService * newService )
{
    IOHIDKeyboard * self = (IOHIDKeyboard *) target;

    IORecursiveLockLock(self->_keyboardLock);
    
    if( OSDynamicCast(IOHIDConsumer,newService) && 
        (self->_consumer != newService) ) 
    {
        
        if( self->_consumer) {
            if (self->_consumer->isDispatcher()) {
                self->_consumer->stop(self);
                self->_consumer->detach(self);
            }
            self->_consumer->release();
        }
        
        self->_consumer = newService;
        self->_consumer->retain();
       
	if ( self->_publishNotify )
	{ 
        	self->_publishNotify->remove();
        	self->_publishNotify = 0;
	}
    }
    
    IORecursiveLockUnlock(self->_keyboardLock);

    return true;
}

void IOHIDKeyboard::stop(IOService * provider)
{    
    if (_asyncLEDThread)
    {
	thread_call_cancel(_asyncLEDThread);
	thread_call_free(_asyncLEDThread);
	_asyncLEDThread = 0;
    }

    IORecursiveLockLock(_keyboardLock);
    if( _consumer) {
        if (_consumer->isDispatcher()) {
            _consumer->stop(this);
            _consumer->detach(this);
        }
        _consumer->release();
        _consumer = 0;
    }
    IORecursiveLockUnlock(_keyboardLock);
    
    super::stop(provider);
}

void IOHIDKeyboard::free()
{
    if (_oldArraySelectors)
    {
        IOFree(_oldArraySelectors, sizeof(UInt32) * _keyCodeArrayValuePtrArray->getCount());
        _oldArraySelectors = 0;
    }

    if (_keyCodeArrayValuePtrArray) 
    {
        _keyCodeArrayValuePtrArray->release();
        _keyCodeArrayValuePtrArray = 0;
    }

    if (_publishNotify) 
    {
        _publishNotify->remove();
    	_publishNotify = 0;
    }
        
    if (_keyboardLock)
    {
        IORecursiveLockLock(_keyboardLock);
        IORecursiveLock* tempLock = _keyboardLock;
	_keyboardLock = NULL;
        IORecursiveLockUnlock(tempLock);
	IORecursiveLockFree(tempLock);
    }
    
    super::free();
}

bool
IOHIDKeyboard::determineKeyboard(IOHIDDevice *owner)
{
    OSDictionary *	pair;
    OSArray *		usagePairs;
    OSNumber *		usagePage;
    OSNumber *		usage;
    UInt32		usageVal, usagePageVal;
    bool		isKeyboard = false;
            
    usagePairs = OSDynamicCast(OSArray, owner->getProperty(kIOHIDDeviceUsagePairsKey));
    
    for (int i=0; usagePairs && i<usagePairs->getCount(); i++)
    {
        if (!(pair = usagePairs->getObject(i)))
            continue;
            
        usagePage 	= pair->getObject(kIOHIDDeviceUsagePageKey);
        usage 		= pair->getObject(kIOHIDDeviceUsageKey);
        
        usagePageVal 	= (usagePage) ? usagePage->unsigned32BitValue() : 0;
        usageVal 	= (usage) ? usage->unsigned32BitValue() : 0;
        
        if ( (usagePageVal == kHIDPage_GenericDesktop) &&
            ((usageVal == kHIDUsage_GD_Keyboard) || (usageVal == kHIDUsage_GD_Keypad)) )
        {
            isKeyboard = true;
            break;
        }
    }

    return isKeyboard;

}

bool
IOHIDKeyboard::findDesiredElements(OSArray *elements, IOHIDDevice *owner)
{
    OSNumber *		fnUsage;
    OSNumber *		fnUsagePage;    
    IOHIDElement *	element;
    UInt32		usage, usagePage;
    UInt32		count;
    
    if (!determineKeyboard(owner))
        return false;

    if (!elements)
        return false;
    
    if (!(_keyCodeArrayValuePtrArray = OSArray::withCapacity(6)))
        return false;
        
    fnUsage = OSDynamicCast(OSNumber, owner->getProperty(kFnModifierUsageKey));
    fnUsagePage = OSDynamicCast(OSNumber,owner->getProperty(kFnModifierUsagePageKey));    
    
    count = elements->getCount();
    for (int i=0; i<count; i++)
    {
        element		= elements->getObject(i);
        usagePage	= element->getUsagePage();
        usage		= element->getUsage();

        if (!_fKeyValuePtr && fnUsage && fnUsagePage &&
            (usage == fnUsage->unsigned32BitValue()) &&
            (usagePage == fnUsagePage->unsigned32BitValue()))
        {
            _fKeyValuePtr = element->getElementValue()->value;
        }                    
        
        switch (usagePage)
        {
            case kHIDPage_KeyboardOrKeypad:
                // Modifier Elements
                if ((usage >= kHIDUsage_KeyboardLeftControl) &&
                    (usage <= kHIDUsage_KeyboardRightGUI) && 
                    (_modifierValuePtrs[usage - kHIDUsage_KeyboardLeftControl] == 0))
                {
                    _modifierValuePtrs[usage - kHIDUsage_KeyboardLeftControl] = 
                                                    element->getElementValue()->value;
                }
                // Key Array Element
                else if ((usage == 0xffffffff) && (element->getReportCount() == 1)) 
                {
                    _keyCodeArrayValuePtrArray->setObject(element);
                }
                break;
                
            case kHIDPage_LEDs:
                if (((usage == kHIDUsage_LED_NumLock) || 
                    (usage == kHIDUsage_LED_CapsLock)) &&
                    (_ledValuePtrs[usage - kHIDUsage_LED_NumLock] == 0))
                {
                    _ledValuePtrs[usage - kHIDUsage_LED_NumLock] = element->getElementValue()->value;
                    _ledCookies[usage - kHIDUsage_LED_NumLock] = element->getElementCookie();
                    _numLeds++;
                }
                break;
            default:
                break;
        }
    }

    UInt32 keyCount = _keyCodeArrayValuePtrArray->getCount();
    if (keyCount)
    {
        _oldArraySelectors = (UInt32 *)IOMalloc(sizeof(UInt32) * keyCount);
                        
        if ( !_oldArraySelectors )
            return false;
                        
        bzero(_oldArraySelectors, sizeof(UInt32) * keyCount);    
    }
    
    return (keyCount);
}

void IOHIDKeyboard::findSecondaryKeys()
{
    OSString *	mappingString;
    char *	str;
    int		i, index, count;
    
    mappingString = OSDynamicCast(OSString,_provider->getProperty(kNumPadUsageMapKey));
    if (mappingString)
    {
        count	= mappingString->getLength();
        str	= mappingString->getCStringNoCopy();
        
        for (i=0; i<count; i+=10)
        {
            index = strtol(&(str[i]), NULL, 16);
            _secondaryKeys[index].bits |= kSecondaryKeyNumPad;
            _secondaryKeys[index].numPadUsage = strtol(&(str[i+5]), NULL, 16);
        }    
    }
    
    mappingString = OSDynamicCast(OSString,_provider->getProperty(kFnSpecialKeyMapKey));
    if (mappingString)
    {
        count	= mappingString->getLength();
        str	= mappingString->getCStringNoCopy();
        
        for (i=0; i<count; i+=10)
        {
            index = strtol(&(str[i]), NULL, 16);
            _secondaryKeys[index].bits |= kSecondaryKeyFnSpecial;
            _secondaryKeys[index].specialKey = strtol(&(str[i+5]), NULL, 16);
        }    
    }

    mappingString = OSDynamicCast(OSString,_provider->getProperty(kFnNonSpecialUsageMapKey));
    if (mappingString)
    {
        count	= mappingString->getLength();
        str	= mappingString->getCStringNoCopy();
        
        for (i=0; i<count; i+=10)
        {
            index = strtol(&(str[i]), NULL, 16);
            _secondaryKeys[index].bits |= kSecondaryKeyFnNonSpecial;
            _secondaryKeys[index].fnUsage = strtol(&(str[i+5]), NULL, 16);
        }    
    }
}

void 
IOHIDKeyboard::handleReport()
{
    UInt8		modifier=0;
    UInt32		alpha = 0;
    bool		found;
    AbsoluteTime	now;
    UInt8		seq_key, i;//counter for alpha keys pressed.
    IOHIDElement *	element;


    // Test for the keyboard bug where all the keys are 0x01. JDC.
    found = true;
    for (seq_key = 0; seq_key < _keyCodeArrayValuePtrArray->getCount(); seq_key++) {
      if ((element = _keyCodeArrayValuePtrArray->getObject(seq_key)) &&
            (element->getElementValue()->value[0] != 1))
            found = false;
    }
    if (found) return;

    clock_get_uptime(&now);

    //Handle new key information.  The first byte is a set of bits describing
    //  which modifier keys are down.  The 2nd byte never seems to be used.
    //  The third byte is the first USB key down, and the fourth byte is the
    //  second key down, and so on.
    //When a key is released, there's no code... just a zero upon USB polling
    //8/2/99 A.W. fixed Blue Box's multiple modifier keys being pressed 
    //   simultaneously.  The trick is if a modifier key DOWN event is reported,
    //   and another DOWN is reported, then Blue Box loses track of it.  I must
    //   report a UP key event first, or else avoid resending the DOWN event.
    
    // Create modifier byte
    for (i = 0; i < 8; i++)
    {
        modifier |= _modifierValuePtrs[i][0] << i;
    }
     
    //SECTION 1. Handle modifier keys here first
    if (modifier == _oldmodifier) 
    {
        //Do nothing.  Same keys are still pressed, or if 0 then none pressed
	// so don't overload the HID system with useless events.
    }
    else //Modifiers may or may not be pressed right now
    {
	//kprintf("mod is %x\n", modifier);

        //left-hand CONTROL modifier key
        if ((modifier & kUSB_LEFT_CONTROL_BIT) && !(_oldmodifier & kUSB_LEFT_CONTROL_BIT))
        {
            dispatchKeyboardEvent(_usb_2_adb_keymap[0xe0], true, now);  //ADB left-hand CONTROL
        }
	else if ((_oldmodifier & kUSB_LEFT_CONTROL_BIT) && !(modifier & kUSB_LEFT_CONTROL_BIT))
	{
	    //Now check for released modifier keys.  Both right and left modifiers must be
	    //   checked otherwise Window Server thinks none are held down
	    dispatchKeyboardEvent(_usb_2_adb_keymap[0xe0], false, now); 
	}

        //right-hand CONTROL modifier
        if ((modifier & kUSB_RIGHT_CONTROL_BIT) && !(_oldmodifier & kUSB_RIGHT_CONTROL_BIT))
        {
            dispatchKeyboardEvent(_usb_2_adb_keymap[0xe4], true, now);  //right-hand CONTROL
        }
	else if ((_oldmodifier & kUSB_RIGHT_CONTROL_BIT) && !(modifier & kUSB_RIGHT_CONTROL_BIT))
	{
	    dispatchKeyboardEvent(_usb_2_adb_keymap[0xe4], false, now); 
	}

        //left-hand SHIFT
        if ((modifier & kUSB_LEFT_SHIFT_BIT) && !(_oldmodifier & kUSB_LEFT_SHIFT_BIT))
        {
            dispatchKeyboardEvent(_usb_2_adb_keymap[0xe1], true, now);
        }
	else if ((_oldmodifier & kUSB_LEFT_SHIFT_BIT) && !(modifier & kUSB_LEFT_SHIFT_BIT))
	{
	    dispatchKeyboardEvent(_usb_2_adb_keymap[0xe1], false, now); 
	}

        //right-hand SHIFT
        if ((modifier & kUSB_RIGHT_SHIFT_BIT) && !(_oldmodifier & kUSB_RIGHT_SHIFT_BIT))
        {
            dispatchKeyboardEvent(_usb_2_adb_keymap[0xe5], true, now);
        }
	else if ((_oldmodifier & kUSB_RIGHT_SHIFT_BIT) && !(modifier & kUSB_RIGHT_SHIFT_BIT))
	{
	    dispatchKeyboardEvent(_usb_2_adb_keymap[0xe5], false, now); 
	}

        if ((modifier & kUSB_LEFT_ALT_BIT) && !(_oldmodifier & kUSB_LEFT_ALT_BIT))
        {
            dispatchKeyboardEvent(_usb_2_adb_keymap[0xe2], true, now);
        }
	else if ((_oldmodifier & kUSB_LEFT_ALT_BIT) && !(modifier & kUSB_LEFT_ALT_BIT))
	{
	    dispatchKeyboardEvent(_usb_2_adb_keymap[0xe2], false, now); 
	}

        if ((modifier & kUSB_RIGHT_ALT_BIT) && !(_oldmodifier & kUSB_RIGHT_ALT_BIT))
        {
            dispatchKeyboardEvent(_usb_2_adb_keymap[0xe6], true, now);
        }
	else if ((_oldmodifier & kUSB_RIGHT_ALT_BIT) && !(modifier & kUSB_RIGHT_ALT_BIT))
	{
	    dispatchKeyboardEvent(_usb_2_adb_keymap[0xe6], false, now); 
	}

        if ((modifier & kUSB_LEFT_FLOWER_BIT) && !(_oldmodifier & kUSB_LEFT_FLOWER_BIT))
        {
            dispatchKeyboardEvent(_usb_2_adb_keymap[0xe3], true, now);
        }
	else if ((_oldmodifier & kUSB_LEFT_FLOWER_BIT) && !(modifier & kUSB_LEFT_FLOWER_BIT))
	{
	    dispatchKeyboardEvent(_usb_2_adb_keymap[0xe3], false, now); 
	}

        if ((modifier & kUSB_RIGHT_FLOWER_BIT) && !(_oldmodifier & kUSB_RIGHT_FLOWER_BIT))
        {
            //dispatchKeyboardEvent(0x7e, true, now);
	    //WARNING... NeXT only recognizes left-hand flower key, so
	    //  emulate that for now
	    dispatchKeyboardEvent(_usb_2_adb_keymap[0xe7], true, now);
        }
	else if ((_oldmodifier & kUSB_RIGHT_FLOWER_BIT) && !(modifier & kUSB_RIGHT_FLOWER_BIT))
	{
	    dispatchKeyboardEvent(_usb_2_adb_keymap[0xe7], false, now); 
	}
    }
    
    if ( _fKeyValuePtr && ( *_fKeyValuePtr != _lastFKeyValue ) )
    {
        _lastFKeyValue = *_fKeyValuePtr;
        dispatchKeyboardEvent(0x3f, _lastFKeyValue, now); 
    }

    //SECTION 2. Handle regular alphanumeric keys now.  Look first at previous keystrokes.
    //  Alphanumeric portion of HID report starts at byte +2.

    for (seq_key = 0; seq_key < _keyCodeArrayValuePtrArray->getCount(); seq_key++)
    {
        alpha = _oldArraySelectors[seq_key];
	if (alpha == 0) //No keys pressed
	{
	    continue;
	}
	found = false;
	for (i = 0; i < _keyCodeArrayValuePtrArray->getCount(); i++)  //Look through current keypresses
	{
            if ((element = _keyCodeArrayValuePtrArray->getObject(i)) &&
                (element->getElementValue()->value[0] == alpha))
	    {
		found = true;	//This key has been held down for a while, so do nothing.
		break;		//   Autorepeat is taken care of by IOKit.
	    }
	}
	if (!found)
	{
          //���  if ( (alpha > 0x58) && ( alpha < 0x63 ) )
          //���      USBLog(3,"Keypad %d pressed",(alpha-0x58));
            if (!filterSecondaryFnSpecialKey(&alpha, false, now))   
                if (!filterSecondaryFnNonSpecialKey(&alpha, false, now))   
                    if (!filterSecondaryNumPadKey(&alpha, false, now))   
                        dispatchKeyboardEvent(_usb_2_adb_keymap[alpha], false, now);
	}
    }

    //Now take care of KEY DOWN.  
    for (seq_key = 0; seq_key < _keyCodeArrayValuePtrArray->getCount(); seq_key++)
    {
        if (!(element = _keyCodeArrayValuePtrArray->getObject(seq_key)))
            continue;
            
        alpha = element->getElementValue()->value[0];
        
        if (alpha == 0) //No keys pressed
        {
            continue;
        }

        //Don't dispatch the same key again which was held down previously
        found = false;
        for (i = 0; i < _keyCodeArrayValuePtrArray->getCount(); i++)
        {
                if (alpha == _oldArraySelectors[i])
            {
            found = true;
            break;
            }
        }
        if (!found)
        {
            //If Debugger() is triggered then I shouldn't show the restart dialog
            //  box, but I think developers doing kernel debugging can live with
            //  this minor incovenience.  Otherwise I need to do more checking here.
                if (!filterSecondaryFnSpecialKey(&alpha, true, now))   
                    if (!filterSecondaryFnNonSpecialKey(&alpha, true, now))   
                        if (!filterSecondaryNumPadKey(&alpha, true, now))   
                            dispatchKeyboardEvent(_usb_2_adb_keymap[alpha], true, now);  //KEY UP
        }
    }

    //Save the history for next time
    _oldmodifier = modifier;
    for (i = 0; i < _keyCodeArrayValuePtrArray->getCount(); i++)
    {
        if (!(element = _keyCodeArrayValuePtrArray->getObject(i)))
            continue;
            
        _oldArraySelectors[i] = element->getElementValue()->value[0];
    }
}

#define SHOULD_SWAP_FN_SPECIAL_KEY(key, down)                   \
    ((_secondaryKeys[key].bits & kSecondaryKeyFnSpecial) &&	\
    (!( _lastFKeyValue ^ _fKeyMode ) ||	(!down &&		\
    (_secondaryKeys[key].swapping & kSecondaryKeyFnSpecial))))

#define SHOULD_SWAP_FN_NUM_PAD_KEY(key, down)                   \
    ((_secondaryKeys[key].bits & kSecondaryKeyFnNonSpecial) &&	\
    (( _lastFKeyValue ^ 					\
    (_fKeyMode && _stickyKeysOn) ) || (!down && \
    (_secondaryKeys[key].swapping & kSecondaryKeyFnNonSpecial))))
    
#define SHOULD_SWAP_NUM_PAD_KEY(key, down)			\
    ((numLock() || ( !down &&					\
    (_secondaryKeys[key].swapping & kSecondaryKeyNumPad))))

bool IOHIDKeyboard::filterSecondaryFnSpecialKey(int * usage, bool down, AbsoluteTime ts)
{
    if ( !_fKeyValuePtr )
        return false;

    if (SHOULD_SWAP_FN_SPECIAL_KEY(*usage, down))
    {
        if (down)
            _secondaryKeys[*usage].swapping |= kSecondaryKeyFnSpecial;
        else
            _secondaryKeys[*usage].swapping = 0; 

        IORecursiveLockLock(_keyboardLock);
        if (!_consumer)
        {
            _consumer = IOHIDConsumer::Dispatcher(this);
            if (_consumer &&
                (!_consumer->attach(this) || 
                    !_consumer->start(this))) 
            {
                _consumer->release();
                _consumer = 0;
            }
        }
        
        if (_consumer)
        {
            _consumer->dispatchSpecialKeyEvent(
                            _secondaryKeys[*usage].specialKey, down, ts);
        }
        IORecursiveLockUnlock(_keyboardLock);
            
        return true;
    }
    
    return false;
}

bool IOHIDKeyboard::filterSecondaryFnNonSpecialKey(int * usage, bool down, AbsoluteTime ts)
{   
    if ( !_fKeyValuePtr )
        return false;

    if (SHOULD_SWAP_FN_NUM_PAD_KEY(*usage, down))
    {
        if (down)
            _secondaryKeys[*usage].swapping |= kSecondaryKeyFnNonSpecial;
        else
            _secondaryKeys[*usage].swapping = 0; 

        *usage = _secondaryKeys[*usage].fnUsage;
    }
    
    return false;
}

bool IOHIDKeyboard::filterSecondaryNumPadKey(int * usage, bool down, AbsoluteTime ts)
{
    if ( !_fKeyValuePtr )
        return false;

    if (SHOULD_SWAP_NUM_PAD_KEY(*usage, down))
    {
        // If the key is not a swapped numpad key, consume it
        if (_secondaryKeys[*usage].bits & kSecondaryKeyNumPad)
        {
            if (down)
                _secondaryKeys[*usage].swapping |= kSecondaryKeyNumPad;
            else
                _secondaryKeys[*usage].swapping = 0; 

            *usage = _secondaryKeys[*usage].numPadUsage;
        }
        else
            return true;
    }

    return false;
}


// **************************************************************************
// _asyncLED
//
// Called asynchronously to turn on/off the keyboard LED
//
// **************************************************************************
void 
IOHIDKeyboard::_asyncLED(OSObject *target)
{
    IOHIDKeyboard *me = OSDynamicCast(IOHIDKeyboard, target);

    me->Set_LED_States( me->_ledState ); 
}

void
IOHIDKeyboard::Set_LED_States(UInt8 ledState)
{
    IOHIDElementCookie	cookies[_numLeds];
    int			cookieCount = 0;
    
    for (int i=0; i<2; i++)
    {
        if (_ledValuePtrs[i])
        {
            _ledValuePtrs[i][0] = (ledState >> i) & 1;
            cookies[cookieCount++] = _ledCookies[i];
        }
    }
    
    _provider->postElementValues(cookies, cookieCount);
}

//******************************************************************************
// COPIED from ADB keyboard driver 3/25/99
// This is usually called on a call-out thread after the caps-lock key is pressed.
// ADB operations to PMU are synchronous, and this is must not be done
// on the call-out thread since that is the PMU driver workloop thread, and
// it will block itself.
//
// Therefore, we schedule the ADB write to disconnect the call-out thread
// and the one that initiates the ADB write.
//
// *******************************************************************************
void
IOHIDKeyboard::setAlphaLockFeedback ( bool LED_state)
{
    //*** TODO *** REVISIT ***
    UInt8	newState = _ledState;

    if (LED_state) //set alpha lock
	newState |= kUSB_CAPSLOCKLED_SET;   //2nd bit is caps lock on USB
    else
	newState &= ~kUSB_CAPSLOCKLED_SET;

    if (newState != _ledState)
    {
        _ledState = newState;
        
        if (_asyncLEDThread) 
            thread_call_enter(_asyncLEDThread);
    }
}



void
IOHIDKeyboard::setNumLockFeedback ( bool LED_state)
{

    //*** TODO *** REVISIT ***
    UInt8	newState = _ledState;

    if (LED_state) 
	newState |= kUSB_NUMLOCKLED_SET;   //1st bit is num lock on USB
    else
	newState &= ~kUSB_NUMLOCKLED_SET;

    if (newState != _ledState)
    {
        _ledState = newState;
        
        if (_asyncLEDThread) 
            thread_call_enter(_asyncLEDThread);
    }
}


//Called from parent classes
unsigned
IOHIDKeyboard::getLEDStatus (void )
{
    unsigned	ledState = 0;
    
    for (int i=0; i<2; i++)
    {
        if (_ledValuePtrs[i])
        {
            ledState |= _ledValuePtrs[i][0] << i;
        }
    }

}

// *****************************************************************************
// maxKeyCodes
// A.W. copied 3/25/99 from ADB keyboard driver, I don't know what this does
// ***************************************************************************
UInt32 
IOHIDKeyboard::maxKeyCodes (void )
{
    return 0x80;
}


// *************************************************************************
// deviceType
//
// **************************************************************************
UInt32 
IOHIDKeyboard::deviceType ( void )
{
    UInt32	id;	
    OSNumber 	*xml_handlerID;

    //Info.plist key is <integer>, not <string>
    xml_handlerID = OSDynamicCast( OSNumber, _provider->getProperty("alt_handler_id"));
    if (xml_handlerID)
    {
	id = xml_handlerID->unsigned32BitValue();
    }
    else
    {
	id = handlerID();
    }

    return id;

}

// ************************************************************************
// interfaceID.  Fake ADB for now since USB defaultKeymapOfLength is too complex
//
// **************************************************************************
UInt32 
IOHIDKeyboard::interfaceID ( void )
{
    //Return value must match "interface" line in .keyboard file
    return NX_EVS_DEVICE_INTERFACE_ADB;  // 2 This matches contents of AppleExt.keyboard
}


/***********************************************/
//Get handler ID 
//
//  I assume that this method is only called if a valid USB keyboard
//  is found. This method should return a 0 or something if there's
//  no keyboard, but then the USB keyboard driver should never have
//  been probed if there's no keyboard, so for now it won't return 0.
UInt32
IOHIDKeyboard::handlerID ( void )
{
    UInt32 ret_id = 2;  //Default for all unknown USB keyboards is 2
    UInt16 productIDVal = _productID ? _productID->unsigned16BitValue() : 0;
    UInt16 vendorIDVal = _vendorID ? _vendorID->unsigned16BitValue() : 0;

    if (vendorIDVal == 0x045e)  //Microsoft ID
    {
        if (productIDVal == 0x000b)   //Natural USB+PS/2 keyboard
            ret_id = 2;  //18 was OSX Server, now 2 is OSX Extended ADB keyboard, unknown manufacturer
    }

    //New feature for hardware identification using Gestalt.h values
    if (vendorIDVal == kIOUSBVendorIDAppleComputer)
        switch (productIDVal)
        {
            case kprodUSBCosmoANSIKbd:  //Cosmo ANSI is 0x201
                    ret_id = kgestUSBCosmoANSIKbd; //0xc6
                    break;
            case kprodUSBCosmoISOKbd:  //Cosmo ISO
                    ret_id = kgestUSBCosmoISOKbd; //0xc7
                    break;
            case kprodUSBCosmoJISKbd:  //Cosmo JIS
                    ret_id = kgestUSBCosmoJISKbd;  //0xc8
                    break;
            case kprodUSBAndyANSIKbd:  //Andy ANSI is 0x204
                    ret_id = kgestUSBAndyANSIKbd; //0xcc
                    break;
            case kprodUSBAndyISOKbd:  //Andy ISO
                    ret_id = kgestUSBAndyISOKbd; //0xcd
                    break;
            case kprodUSBAndyJISKbd:  //Andy JIS is 0x206
                    ret_id = kgestUSBAndyJISKbd; //0xce
                    break;
            case kprodQ6ANSIKbd:  //Q6 ANSI
                    ret_id = kgestQ6ANSIKbd;
                    break;
            case kprodQ6ISOKbd:  //Q6 ISO
                    ret_id = kgestQ6ISOKbd;
                    break;
            case kprodQ6JISKbd:  //Q6 JIS
                    ret_id = kgestQ6JISKbd;
                    break;
            case kprodQ30ANSIKbd:  //Q30 ANSI
                    ret_id = kgestQ30ANSIKbd;
                    break;
            case kprodQ30ISOKbd:  //Q30 ISO
                    ret_id = kgestQ30ISOKbd;
                    break;
            case kprodQ30JISKbd:  //Q30 JIS
                    ret_id = kgestQ30JISKbd;
                    break;
            case kprodFountainANSIKbd:  //Fountain ANSI
                    ret_id = kgestFountainANSIKbd;
                    break;
            case kprodFountainISOKbd:  //Fountain ISO
                    ret_id = kgestFountainISOKbd;
                    break;
            case kprodFountainJISKbd:  //Fountain JIS
                    ret_id = kgestFountainJISKbd;
                    break;
            case kprodSantaANSIKbd:  //Santa ANSI
                    ret_id = kgestSantaANSIKbd;
                    break;
            case kprodSantaISOKbd:  //Santa ISO
                    ret_id = kgestSantaISOKbd;
                    break;
            case kprodSantaJISKbd:  //Santa JIS
                    ret_id = kgestSantaJISKbd;
                    break;
                    
            
            default:  // No Gestalt.h values, but still is Apple keyboard,
                    //   so return a generic Cosmo ANSI
                    ret_id = kgestUSBCosmoANSIKbd;  
                    break;
        }

    return ret_id;  //non-Apple USB keyboards should all return "2"
}


// *****************************************************************************
// defaultKeymapOfLength
// A.W. copied from ADB keyboard, I don't have time to make custom USB version
// *****************************************************************************
const unsigned char * 
IOHIDKeyboard::defaultKeymapOfLength (UInt32 * length )
{
    if ( _fKeyValuePtr )
    {
        // this one defines the FKeyMap
        static const unsigned char appleUSAFKeyMap[] = {
            0x00,0x00,
            
            // Modifier Defs
            0x0b,   //Number of modifier keys.  Was 7
            //0x00,0x01,0x39,  //CAPSLOCK, uses one byte.
            0x01,0x01,0x38,
            0x02,0x01,0x3b,
            0x03,0x01,0x3a,
            0x04,0x01,0x37,
            0x05,0x10,0x52,0x41,0x53,0x54,0x55,0x45,0x58,0x57,0x56,0x5b,0x5c,
            0x43,0x4b,0x51,0x4e,0x59,
            0x06,0x01,0x72,
            0x07,0x01,0x3f, //NX_MODIFIERKEY_SECONDARYFN 8th modifier
            0x09,0x01,0x3c, //Right shift
            0x0a,0x01,0x3e, //Right control
            0x0b,0x01,0x3d, //Right Option
            0x0c,0x01,0x36, //Right Command
            
            // key deffs
            0x7f,0x0d,0x00,0x61,
            0x00,0x41,0x00,0x01,0x00,0x01,0x00,0xca,0x00,0xc7,0x00,0x01,0x00,0x01,0x0d,0x00,
            0x73,0x00,0x53,0x00,0x13,0x00,0x13,0x00,0xfb,0x00,0xa7,0x00,0x13,0x00,0x13,0x0d,
            0x00,0x64,0x00,0x44,0x00,0x04,0x00,0x04,0x01,0x44,0x01,0xb6,0x00,0x04,0x00,0x04,
            0x0d,0x00,0x66,0x00,0x46,0x00,0x06,0x00,0x06,0x00,0xa6,0x01,0xac,0x00,0x06,0x00,
                0x06,0x0d,0x00,0x68,0x00,0x48,0x00,0x08,0x00,0x08,0x00,0xe3,0x00,0xeb,0x00,0x00,
                0x18,0x00,0x0d,0x00,0x67,0x00,0x47,0x00,0x07,0x00,0x07,0x00,0xf1,0x00,0xe1,0x00,
                0x07,0x00,0x07,0x0d,0x00,0x7a,0x00,0x5a,0x00,0x1a,0x00,0x1a,0x00,0xcf,0x01,0x57,
                0x00,0x1a,0x00,0x1a,0x0d,0x00,0x78,0x00,0x58,0x00,0x18,0x00,0x18,0x01,0xb4,0x01,
                0xce,0x00,0x18,0x00,0x18,0x0d,0x00,0x63,0x00,0x43,0x00,0x03,0x00,0x03,0x01,0xe3,
                0x01,0xd3,0x00,0x03,0x00,0x03,0x0d,0x00,0x76,0x00,0x56,0x00,0x16,0x00,0x16,0x01,
                0xd6,0x01,0xe0,0x00,0x16,0x00,0x16,0x02,0x00,0x3c,0x00,0x3e,0x0d,0x00,0x62,0x00,
                0x42,0x00,0x02,0x00,0x02,0x01,0xe5,0x01,0xf2,0x00,0x02,0x00,0x02,0x0d,0x00,0x71,
                0x00,0x51,0x00,0x11,0x00,0x11,0x00,0xfa,0x00,0xea,0x00,0x11,0x00,0x11,0x0d,0x00,
                0x77,0x00,0x57,0x00,0x17,0x00,0x17,0x01,0xc8,0x01,0xc7,0x00,0x17,0x00,0x17,0x0d,
                0x00,0x65,0x00,0x45,0x00,0x05,0x00,0x05,0x00,0xc2,0x00,0xc5,0x00,0x05,0x00,0x05,
                0x0d,0x00,0x72,0x00,0x52,0x00,0x12,0x00,0x12,0x01,0xe2,0x01,0xd2,0x00,0x12,0x00,
                0x12,0x0d,0x00,0x79,0x00,0x59,0x00,0x19,0x00,0x19,0x00,0xa5,0x01,0xdb,0x00,0x19,
                0x00,0x19,0x0d,0x00,0x74,0x00,0x54,0x00,0x14,0x00,0x14,0x01,0xe4,0x01,0xd4,0x00,
                0x14,0x00,0x14,0x0a,0x00,0x31,0x00,0x21,0x01,0xad,0x00,0xa1,0x0e,0x00,0x32,0x00,
                0x40,0x00,0x32,0x00,0x00,0x00,0xb2,0x00,0xb3,0x00,0x00,0x00,0x00,0x0a,0x00,0x33,
                0x00,0x23,0x00,0xa3,0x01,0xba,0x0a,0x00,0x34,0x00,0x24,0x00,0xa2,0x00,0xa8,0x0e,
                0x00,0x36,0x00,0x5e,0x00,0x36,0x00,0x1e,0x00,0xb6,0x00,0xc3,0x00,0x1e,0x00,0x1e,
                0x0a,0x00,0x35,0x00,0x25,0x01,0xa5,0x00,0xbd,0x0a,0x00,0x3d,0x00,0x2b,0x01,0xb9,
                0x01,0xb1,0x0a,0x00,0x39,0x00,0x28,0x00,0xac,0x00,0xab,0x0a,0x00,0x37,0x00,0x26,
                0x01,0xb0,0x01,0xab,0x0e,0x00,0x2d,0x00,0x5f,0x00,0x1f,0x00,0x1f,0x00,0xb1,0x00,
                0xd0,0x00,0x1f,0x00,0x1f,0x0a,0x00,0x38,0x00,0x2a,0x00,0xb7,0x00,0xb4,0x0a,0x00,
                0x30,0x00,0x29,0x00,0xad,0x00,0xbb,0x0e,0x00,0x5d,0x00,0x7d,0x00,0x1d,0x00,0x1d,
                0x00,0x27,0x00,0xba,0x00,0x1d,0x00,0x1d,0x0d,0x00,0x6f,0x00,0x4f,0x00,0x0f,0x00,
                0x0f,0x00,0xf9,0x00,0xe9,0x00,0x0f,0x00,0x0f,0x0d,0x00,0x75,0x00,0x55,0x00,0x15,
                0x00,0x15,0x00,0xc8,0x00,0xcd,0x00,0x15,0x00,0x15,0x0e,0x00,0x5b,0x00,0x7b,0x00,
                0x1b,0x00,0x1b,0x00,0x60,0x00,0xaa,0x00,0x1b,0x00,0x1b,0x0d,0x00,0x69,0x00,0x49,
                0x00,0x09,0x00,0x09,0x00,0xc1,0x00,0xf5,0x00,0x09,0x00,0x09,0x0d,0x00,0x70,0x00,
                0x50,0x00,0x10,0x00,0x10,0x01,0x70,0x01,0x50,0x00,0x10,0x00,0x10,0x10,0x00,0x0d,
                0x00,0x03,0x0d,0x00,0x6c,0x00,0x4c,0x00,0x0c,0x00,0x0c,0x00,0xf8,0x00,0xe8,0x00,
                0x0c,0x00,0x0c,0x0d,0x00,0x6a,0x00,0x4a,0x00,0x0a,0x00,0x0a,0x00,0xc6,0x00,0xae,
                0x00,0x0a,0x00,0x0a,0x0a,0x00,0x27,0x00,0x22,0x00,0xa9,0x01,0xae,0x0d,0x00,0x6b,
                0x00,0x4b,0x00,0x0b,0x00,0x0b,0x00,0xce,0x00,0xaf,0x00,0x0b,0x00,0x0b,0x0a,0x00,
                0x3b,0x00,0x3a,0x01,0xb2,0x01,0xa2,0x0e,0x00,0x5c,0x00,0x7c,0x00,0x1c,0x00,0x1c,
                0x00,0xe3,0x00,0xeb,0x00,0x1c,0x00,0x1c,0x0a,0x00,0x2c,0x00,0x3c,0x00,0xcb,0x01,
                0xa3,0x0a,0x00,0x2f,0x00,0x3f,0x01,0xb8,0x00,0xbf,0x0d,0x00,0x6e,0x00,0x4e,0x00,
                0x0e,0x00,0x0e,0x00,0xc4,0x01,0xaf,0x00,0x0e,0x00,0x0e,0x0d,0x00,0x6d,0x00,0x4d,
                0x00,0x0d,0x00,0x0d,0x01,0x6d,0x01,0xd8,0x00,0x0d,0x00,0x0d,0x0a,0x00,0x2e,0x00,
                0x3e,0x00,0xbc,0x01,0xb3,0x02,0x00,0x09,0x00,0x19,0x0c,0x00,0x20,0x00,0x00,0x00,
                0x80,0x00,0x00,0x0a,0x00,0x60,0x00,0x7e,0x00,0x60,0x01,0xbb,0x02,0x00,0x7f,0x00,
                0x08,0xff,0x02,0x00,0x1b,0x00,0x7e,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
                0xff,0xff,0x00,0x00,0x2e,0xff,0x00,0x00,
                0x2a,0xff,0x00,0x00,0x2b,0xff,0x00,0x00,0x1b,0xff,0xff,0xff,0x0e,0x00,0x2f,0x00,
                0x5c,0x00,0x2f,0x00,0x1c,0x00,0x2f,0x00,0x5c,0x00,0x00,0x0a,0x00,0x00,0x00,0x0d, //XX03
                0xff,0x00,0x00,0x2d,0xff,0xff,0x0e,0x00,0x3d,0x00,0x7c,0x00,0x3d,0x00,0x1c,0x00,
                0x3d,0x00,0x7c,0x00,0x00,0x18,0x46,0x00,0x00,0x30,0x00,0x00,0x31,0x00,0x00,0x32,
                0x00,0x00,0x33,0x00,0x00,0x34,0x00,0x00,0x35,0x00,0x00,0x36,0x00,0x00,0x37,0xff,
                0x00,0x00,0x38,0x00,0x00,0x39,0xff,0xff,0xff,0x00,0xfe,0x24,0x00,0xfe,0x25,0x00,
                0xfe,0x26,0x00,0xfe,0x22,0x00,0xfe,0x27,0x00,0xfe,0x28,0xff,0x00,0xfe,0x2a,0xff,
                0x00,0xfe,0x32,0x00,0xfe,0x35,0x00,0xfe,0x33,0xff,0x00,0xfe,0x29,0xff,0x00,0xfe,0x2b,0xff,
                0x00,0xfe,0x34,0xff,0x00,0xfe,0x2e,0x00,0xfe,0x30,0x00,0xfe,0x2d,0x00,0xfe,0x23,
                0x00,0xfe,0x2f,0x00,0xfe,0x21,0x00,0xfe,0x31,0x00,0xfe,0x20,
                0x00,0x01,0xac, //ADB=0x7b is left arrow
                0x00,0x01,0xae, //ADB = 0x7c is right arrow
                0x00,0x01,0xaf, //ADB=0x7d is down arrow.  
                0x00,0x01,0xad, //ADB=0x7e is up arrow	 
                0x0f,0x02,0xff,0x04,            
                0x00,0x31,0x02,0xff,0x04,0x00,0x32,0x02,0xff,0x04,0x00,0x33,0x02,0xff,0x04,0x00,
                0x34,0x02,0xff,0x04,0x00,0x35,0x02,0xff,0x04,0x00,0x36,0x02,0xff,0x04,0x00,0x37,
                0x02,0xff,0x04,0x00,0x38,0x02,0xff,0x04,0x00,0x39,0x02,0xff,0x04,0x00,0x30,0x02,
                0xff,0x04,0x00,0x2d,0x02,0xff,0x04,0x00,0x3d,0x02,0xff,0x04,0x00,0x70,0x02,0xff,
                0x04,0x00,0x5d,0x02,0xff,0x04,0x00,0x5b,
                0x07, // following are 7 special keys
                0x04,0x39,  //caps lock
                0x05,0x72,  //NX_KEYTYPE_HELP is 5, ADB code is 0x72
                0x06,0x7f,  //NX_POWER_KEY is 6, ADB code is 0x7f
                0x07,0x4a,  //NX_KEYTYPE_MUTE is 7, ADB code is 0x4a
                0x00,0x48,  //NX_KEYTYPE_SOUND_UP is 0, ADB code is 0x48
                0x01,0x49,  //NX_KEYTYPE_SOUND_DOWN is 1, ADB code is 0x49
                0x0a,0x47   //NX_KEYTYPE_NUM_LOCK is 10, ADB combines with CLEAR key for numlock
        };
        *length = sizeof(appleUSAFKeyMap);
        return appleUSAFKeyMap;
    } 
        
    static const unsigned char appleUSAKeyMap[] = {
        0x00,0x00,
        
        // Modifier Defs
        0x0a,   //Number of modifier keys.  Was 7
        //0x00,0x01,0x39,  //CAPSLOCK, uses one byte.
        0x01,0x01,0x38,
        0x02,0x01,0x3b,
        0x03,0x01,0x3a,
        0x04,0x01,0x37,
        0x05,0x11,0x52,0x41,0x4c,0x53,0x54,0x55,0x45,0x58,0x57,0x56,0x5b,0x5c,
        0x43,0x4b,0x51,0x4e,0x59,
        0x06,0x01,0x72,
        0x09,0x01,0x3c, //Right shift
        0x0a,0x01,0x3e, //Right control
        0x0b,0x01,0x3d, //Right Option
        0x0c,0x01,0x36, //Right Command
        
        // key deffs
        0x7f,0x0d,0x00,0x61,
        0x00,0x41,0x00,0x01,0x00,0x01,0x00,0xca,0x00,0xc7,0x00,0x01,0x00,0x01,0x0d,0x00,
        0x73,0x00,0x53,0x00,0x13,0x00,0x13,0x00,0xfb,0x00,0xa7,0x00,0x13,0x00,0x13,0x0d,
        0x00,0x64,0x00,0x44,0x00,0x04,0x00,0x04,0x01,0x44,0x01,0xb6,0x00,0x04,0x00,0x04,
        0x0d,0x00,0x66,0x00,0x46,0x00,0x06,0x00,0x06,0x00,0xa6,0x01,0xac,0x00,0x06,0x00,
            0x06,0x0d,0x00,0x68,0x00,0x48,0x00,0x08,0x00,0x08,0x00,0xe3,0x00,0xeb,0x00,0x00,
            0x18,0x00,0x0d,0x00,0x67,0x00,0x47,0x00,0x07,0x00,0x07,0x00,0xf1,0x00,0xe1,0x00,
            0x07,0x00,0x07,0x0d,0x00,0x7a,0x00,0x5a,0x00,0x1a,0x00,0x1a,0x00,0xcf,0x01,0x57,
            0x00,0x1a,0x00,0x1a,0x0d,0x00,0x78,0x00,0x58,0x00,0x18,0x00,0x18,0x01,0xb4,0x01,
            0xce,0x00,0x18,0x00,0x18,0x0d,0x00,0x63,0x00,0x43,0x00,0x03,0x00,0x03,0x01,0xe3,
            0x01,0xd3,0x00,0x03,0x00,0x03,0x0d,0x00,0x76,0x00,0x56,0x00,0x16,0x00,0x16,0x01,
            0xd6,0x01,0xe0,0x00,0x16,0x00,0x16,0x02,0x00,0x3c,0x00,0x3e,0x0d,0x00,0x62,0x00,
            0x42,0x00,0x02,0x00,0x02,0x01,0xe5,0x01,0xf2,0x00,0x02,0x00,0x02,0x0d,0x00,0x71,
            0x00,0x51,0x00,0x11,0x00,0x11,0x00,0xfa,0x00,0xea,0x00,0x11,0x00,0x11,0x0d,0x00,
            0x77,0x00,0x57,0x00,0x17,0x00,0x17,0x01,0xc8,0x01,0xc7,0x00,0x17,0x00,0x17,0x0d,
            0x00,0x65,0x00,0x45,0x00,0x05,0x00,0x05,0x00,0xc2,0x00,0xc5,0x00,0x05,0x00,0x05,
            0x0d,0x00,0x72,0x00,0x52,0x00,0x12,0x00,0x12,0x01,0xe2,0x01,0xd2,0x00,0x12,0x00,
            0x12,0x0d,0x00,0x79,0x00,0x59,0x00,0x19,0x00,0x19,0x00,0xa5,0x01,0xdb,0x00,0x19,
            0x00,0x19,0x0d,0x00,0x74,0x00,0x54,0x00,0x14,0x00,0x14,0x01,0xe4,0x01,0xd4,0x00,
            0x14,0x00,0x14,0x0a,0x00,0x31,0x00,0x21,0x01,0xad,0x00,0xa1,0x0e,0x00,0x32,0x00,
            0x40,0x00,0x32,0x00,0x00,0x00,0xb2,0x00,0xb3,0x00,0x00,0x00,0x00,0x0a,0x00,0x33,
            0x00,0x23,0x00,0xa3,0x01,0xba,0x0a,0x00,0x34,0x00,0x24,0x00,0xa2,0x00,0xa8,0x0e,
            0x00,0x36,0x00,0x5e,0x00,0x36,0x00,0x1e,0x00,0xb6,0x00,0xc3,0x00,0x1e,0x00,0x1e,
            0x0a,0x00,0x35,0x00,0x25,0x01,0xa5,0x00,0xbd,0x0a,0x00,0x3d,0x00,0x2b,0x01,0xb9,
            0x01,0xb1,0x0a,0x00,0x39,0x00,0x28,0x00,0xac,0x00,0xab,0x0a,0x00,0x37,0x00,0x26,
            0x01,0xb0,0x01,0xab,0x0e,0x00,0x2d,0x00,0x5f,0x00,0x1f,0x00,0x1f,0x00,0xb1,0x00,
            0xd0,0x00,0x1f,0x00,0x1f,0x0a,0x00,0x38,0x00,0x2a,0x00,0xb7,0x00,0xb4,0x0a,0x00,
            0x30,0x00,0x29,0x00,0xad,0x00,0xbb,0x0e,0x00,0x5d,0x00,0x7d,0x00,0x1d,0x00,0x1d,
            0x00,0x27,0x00,0xba,0x00,0x1d,0x00,0x1d,0x0d,0x00,0x6f,0x00,0x4f,0x00,0x0f,0x00,
            0x0f,0x00,0xf9,0x00,0xe9,0x00,0x0f,0x00,0x0f,0x0d,0x00,0x75,0x00,0x55,0x00,0x15,
            0x00,0x15,0x00,0xc8,0x00,0xcd,0x00,0x15,0x00,0x15,0x0e,0x00,0x5b,0x00,0x7b,0x00,
            0x1b,0x00,0x1b,0x00,0x60,0x00,0xaa,0x00,0x1b,0x00,0x1b,0x0d,0x00,0x69,0x00,0x49,
            0x00,0x09,0x00,0x09,0x00,0xc1,0x00,0xf5,0x00,0x09,0x00,0x09,0x0d,0x00,0x70,0x00,
            0x50,0x00,0x10,0x00,0x10,0x01,0x70,0x01,0x50,0x00,0x10,0x00,0x10,0x10,0x00,0x0d,
            0x00,0x03,0x0d,0x00,0x6c,0x00,0x4c,0x00,0x0c,0x00,0x0c,0x00,0xf8,0x00,0xe8,0x00,
            0x0c,0x00,0x0c,0x0d,0x00,0x6a,0x00,0x4a,0x00,0x0a,0x00,0x0a,0x00,0xc6,0x00,0xae,
            0x00,0x0a,0x00,0x0a,0x0a,0x00,0x27,0x00,0x22,0x00,0xa9,0x01,0xae,0x0d,0x00,0x6b,
            0x00,0x4b,0x00,0x0b,0x00,0x0b,0x00,0xce,0x00,0xaf,0x00,0x0b,0x00,0x0b,0x0a,0x00,
            0x3b,0x00,0x3a,0x01,0xb2,0x01,0xa2,0x0e,0x00,0x5c,0x00,0x7c,0x00,0x1c,0x00,0x1c,
            0x00,0xe3,0x00,0xeb,0x00,0x1c,0x00,0x1c,0x0a,0x00,0x2c,0x00,0x3c,0x00,0xcb,0x01,
            0xa3,0x0a,0x00,0x2f,0x00,0x3f,0x01,0xb8,0x00,0xbf,0x0d,0x00,0x6e,0x00,0x4e,0x00,
            0x0e,0x00,0x0e,0x00,0xc4,0x01,0xaf,0x00,0x0e,0x00,0x0e,0x0d,0x00,0x6d,0x00,0x4d,
            0x00,0x0d,0x00,0x0d,0x01,0x6d,0x01,0xd8,0x00,0x0d,0x00,0x0d,0x0a,0x00,0x2e,0x00,
            0x3e,0x00,0xbc,0x01,0xb3,0x02,0x00,0x09,0x00,0x19,0x0c,0x00,0x20,0x00,0x00,0x00,
            0x80,0x00,0x00,0x0a,0x00,0x60,0x00,0x7e,0x00,0x60,0x01,0xbb,0x02,0x00,0x7f,0x00,
            0x08,0xff,0x02,0x00,0x1b,0x00,0x7e,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0x00,0x00,0x2e,0xff,0x00,0x00,
            0x2a,0xff,0x00,0x00,0x2b,0xff,0x00,0x00,0x1b,0xff,0xff,0xff,0x0e,0x00,0x2f,0x00,
            0x5c,0x00,0x2f,0x00,0x1c,0x00,0x2f,0x00,0x5c,0x00,0x00,0x0a,0x00,0x00,0x00,0x0d, //XX03
            0xff,0x00,0x00,0x2d,0xff,0xff,0x0e,0x00,0x3d,0x00,0x7c,0x00,0x3d,0x00,0x1c,0x00,
            0x3d,0x00,0x7c,0x00,0x00,0x18,0x46,0x00,0x00,0x30,0x00,0x00,0x31,0x00,0x00,0x32,
            0x00,0x00,0x33,0x00,0x00,0x34,0x00,0x00,0x35,0x00,0x00,0x36,0x00,0x00,0x37,0xff,
            0x00,0x00,0x38,0x00,0x00,0x39,0xff,0xff,0xff,0x00,0xfe,0x24,0x00,0xfe,0x25,0x00,
            0xfe,0x26,0x00,0xfe,0x22,0x00,0xfe,0x27,0x00,0xfe,0x28,0xff,0x00,0xfe,0x2a,0xff,
            0x00,0xfe,0x32,0x00,0xfe,0x35,0x00,0xfe,0x33,0xff,0x00,0xfe,0x29,0xff,0x00,0xfe,0x2b,0xff,
            0x00,0xfe,0x34,0xff,0x00,0xfe,0x2e,0x00,0xfe,0x30,0x00,0xfe,0x2d,0x00,0xfe,0x23,
            0x00,0xfe,0x2f,0x00,0xfe,0x21,0x00,0xfe,0x31,0x00,0xfe,0x20,
            0x00,0x01,0xac, //ADB=0x7b is left arrow
            0x00,0x01,0xae, //ADB = 0x7c is right arrow
            0x00,0x01,0xaf, //ADB=0x7d is down arrow.  
            0x00,0x01,0xad, //ADB=0x7e is up arrow	 
            0x0f,0x02,0xff,0x04,            
            0x00,0x31,0x02,0xff,0x04,0x00,0x32,0x02,0xff,0x04,0x00,0x33,0x02,0xff,0x04,0x00,
            0x34,0x02,0xff,0x04,0x00,0x35,0x02,0xff,0x04,0x00,0x36,0x02,0xff,0x04,0x00,0x37,
            0x02,0xff,0x04,0x00,0x38,0x02,0xff,0x04,0x00,0x39,0x02,0xff,0x04,0x00,0x30,0x02,
            0xff,0x04,0x00,0x2d,0x02,0xff,0x04,0x00,0x3d,0x02,0xff,0x04,0x00,0x70,0x02,0xff,
            0x04,0x00,0x5d,0x02,0xff,0x04,0x00,0x5b,
            0x07, // following are 7 special keys
            0x04,0x39,  //caps lock
            0x05,0x72,  //NX_KEYTYPE_HELP is 5, ADB code is 0x72
            0x06,0x7f,  //NX_POWER_KEY is 6, ADB code is 0x7f
            0x07,0x4a,  //NX_KEYTYPE_MUTE is 7, ADB code is 0x4a
            0x00,0x48,  //NX_KEYTYPE_SOUND_UP is 0, ADB code is 0x48
            0x01,0x49,  //NX_KEYTYPE_SOUND_DOWN is 1, ADB code is 0x49
            0x0a,0x47   //NX_KEYTYPE_NUM_LOCK is 10, ADB combines with CLEAR key for numlock
    };
    *length = sizeof(appleUSAKeyMap);
    return appleUSAKeyMap;    

}

//====================================================================================================
// setParamProperties
//====================================================================================================
IOReturn IOHIDKeyboard::setParamProperties( OSDictionary * dict )
{
    OSNumber 	*datan;

    if (_keyboardLock)
	IORecursiveLockLock (_keyboardLock);
    
    if ( _fKeyValuePtr )
    {
        if (datan = OSDynamicCast(OSNumber, dict->getObject(kIOHIDFKeyModeKey)))
        {	
            _fKeyMode = datan->unsigned8BitValue();
            setProperty(kIOHIDFKeyModeKey, datan);
        }
    }
    
    if (datan = OSDynamicCast(OSNumber, dict->getObject(kIOHIDStickyKeysOnKey)))
    {	
	_stickyKeysOn = datan->unsigned8BitValue();
    }

    if (_keyboardLock)
	IORecursiveLockUnlock (_keyboardLock);
        
    return super::setParamProperties(dict);
    
}

//---------------------------------------------------------------------------
// Compare the properties in the supplied table to this object's properties.

static bool CompareProperty( IOService * owner, OSDictionary * matching, const char * key )
{
    // We return success if we match the key in the dictionary with the key in
    // the property table, or if the prop isn't present
    //
    OSObject 	* value;
    bool	matches;
    
    value = matching->getObject( key );

    if( value)
        matches = value->isEqualTo( owner->getProperty( key ));
    else
        matches = true;

    return matches;
}

//====================================================================================================
// matchPropertyTable
//====================================================================================================
bool IOHIDKeyboard::matchPropertyTable(OSDictionary * table, SInt32 * score)
{
    bool match = true;

    // Ask our superclass' opinion.
    if (super::matchPropertyTable(table, score) == false)  return false;

    // Compare properties.        
    if (!CompareProperty(this, table, kIOHIDLocationIDKey) 	||
        !CompareProperty(this, table, kIOHIDTransportKey) 	||
        !CompareProperty(this, table, kIOHIDVendorIDKey) 	||
        !CompareProperty(this, table, kIOHIDProductIDKey))
        match = false;

    return match;
}