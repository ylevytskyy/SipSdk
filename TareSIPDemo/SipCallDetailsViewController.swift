//
//  SipCallDetailsViewController.swift
//  TareSIPDemo
//
//  Created by Yuriy Levytskyy on 2/28/19.
//  Copyright © 2019 Yuriy Levytskyy. All rights reserved.
//

import UIKit

class SipCallDetailsViewController : UIViewController {
    var sipCall: SipCall!

    @IBAction func hold(_ sender: Any) {
        sipCall.holdAnswer()
    }
    
    @IBAction func hangup(_ sender: Any) {
        sipCall.hangup(487, reason: "Request Terminated")
        
        navigationController?.popViewController(animated: true)
    }
}
