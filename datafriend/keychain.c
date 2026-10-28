#ifdef __APPLE__
#include <Security/Security.h>
#include <stdint.h>
#include <string.h>
static const char *load(CFMutableDictionaryRef query, uint8_t seed[32], bool *found) {
  *found = false;
  CFTypeRef result = NULL;
  CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
  OSStatus status = SecItemCopyMatching(query, &result);
  CFDictionaryRemoveValue(query, kSecReturnData);
  const char *error = NULL;
  if (status != errSecItemNotFound) {
    if (status != errSecSuccess)
      error = "Datafriend Keychain read failed";
    else if (!result || CFGetTypeID(result) != CFDataGetTypeID() ||
             CFDataGetLength((CFDataRef)result) != 32)
      error = "invalid Datafriend Keychain identity";
    else {
      memcpy(seed, CFDataGetBytePtr((CFDataRef)result), 32);
      *found = true;
    }
  }
  if (result)
    CFRelease(result);
  return error;
}
const char *df_identity_load(const char *alias, uint8_t seed[32]) {
  const char *error = "out of memory";
  bool found = false;
  CFStringRef account = CFStringCreateWithCString(NULL, alias, kCFStringEncodingUTF8);
  CFMutableDictionaryRef query = NULL;
  CFDataRef secret = NULL;
  if (!account)
    goto done;
  query = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                    &kCFTypeDictionaryValueCallBacks);
  if (!query)
    goto done;
  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, CFSTR("dev.drawdo.datafriend.ed25519.v1"));
  CFDictionarySetValue(query, kSecAttrAccount, account);
  CFDictionarySetValue(query, kSecAttrSynchronizable, kCFBooleanFalse);
  error = load(query, seed, &found);
  if (error || found)
    goto done;
  secret = CFDataCreateWithBytesNoCopy(NULL, seed, 32, kCFAllocatorNull);
  if (!secret) {
    error = "out of memory";
    goto done;
  }
  CFDictionarySetValue(query, kSecValueData, secret);
  CFDictionarySetValue(query, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
  OSStatus status = SecItemAdd(query, NULL);
  CFDictionaryRemoveValue(query, kSecValueData);
  CFDictionaryRemoveValue(query, kSecAttrAccessible);
  if (status == errSecDuplicateItem) {
    error = load(query, seed, &found);
    if (!error && !found)
      error = "identity disappeared during creation";
  } else if (status != errSecSuccess)
    error = "Datafriend Keychain write failed";
done:
  if (secret)
    CFRelease(secret);
  if (query)
    CFRelease(query);
  if (account)
    CFRelease(account);
  return error;
}
#endif
