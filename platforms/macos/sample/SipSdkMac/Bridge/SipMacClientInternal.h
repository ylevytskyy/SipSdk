#import "SipMacClient.h"

NS_ASSUME_NONNULL_BEGIN

struct sip_call_t;
struct sip_account_t;

@interface SipMacClient (InternalBridge)
- (nullable SipMacCall *)getOrCreateCall:(struct sip_call_t *)handle;
- (nullable SipMacAccount *)accountForHandle:(struct sip_account_t *)handle;
@end

NS_ASSUME_NONNULL_END
