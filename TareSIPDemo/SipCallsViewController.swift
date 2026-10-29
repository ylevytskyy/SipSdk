//
//  SipCallsViewController.swift
//  TareSIPDemo
//
//  Created by Yuriy Levytskyy on 2/26/19.
//  Copyright © 2019 Yuriy Levytskyy. All rights reserved.
//

import UIKit

class SipCallsViewController: UIViewController {
    @IBOutlet weak var remoteUri: UITextField!
    @IBOutlet weak var callsTableView: UITableView!
    @IBOutlet weak var logsTextView: UITextView!
    
    private var client = SipClient(username: "YOUR_SIP_USERNAME", domain: "YOUR_SIP_DOMAIN")
    private var calls = Set<SipCall>()
    
    @IBAction func connect(_ sender: Any) {
        guard let uri = remoteUri.text?.trimmingCharacters(in: .whitespacesAndNewlines),
              !uri.isEmpty else {
            logsTextView.text += "Enter a SIP URI before starting a call.\n"
            return
        }

        guard let sipCall = client.makeCall(uri) else {
            logsTextView.text += "Failed to create call. Check SIP registration and the destination URI.\n"
            return
        }

        calls.insert(sipCall)
        callsTableView.reloadData()
    }

    @IBAction func register(_ sender: Any) {
        let result = client.registry()
        if result != 0 {
            logsTextView.text += "Unable to register SIP client (code \(result)).\n"
        }
    }
    
    @IBAction func unregister(_ sender: Any) {
        client.unregister();
    }
    
    override func viewDidLoad() {
        super.viewDidLoad()
        
        callsTableView.dataSource = self
        
        client.delegate = self
        client.password = "YOUR_SIP_PASSWORD"
        let result = client.start()
        if result != 0 {
            logsTextView.text += "Failed to start SIP client (code \(result)). Check the local SIP configuration.\n"
        }
    }
    
    override func prepare(for segue: UIStoryboardSegue, sender: Any?) {
        if segue.identifier == "sipCallDetailsSegue" {
            let calls = Array(self.calls)
            let path = callsTableView.indexPathForSelectedRow!
            let destination = segue.destination as! SipCallDetailsViewController
            destination.sipCall = calls[path.row]
        }
    }
}

extension SipCallsViewController : SipClientDelegate {
    func onWillRegister(_ sipSdk: SipClient) {
        logsTextView.text += "Registering...\n";
    }
    
    func onDidRegister(_ sipSdk: SipClient) {
        logsTextView.text += "Registered\n";
    }
    
    func onFailedRegister(_ sipSdk: SipClient) {
        logsTextView.text += "Failed to register\n";
    }
    
    func onWillUnRegister(_ sipSdk: SipClient) {
        logsTextView.text += "Unregistering...\n";
    }
    
    func onCallIncoming(_ sipCall: SipCall) {
        logsTextView.text += "Incomming call from \(sipCall.remoteUri)...\n";
        
        let alertController = UIAlertController(title: sipCall.remoteUri, message: "Incoming Call", preferredStyle: .alert)
        alertController.addAction(UIAlertAction(title: "Answer", style: .default) { _ in
            sipCall.answer()
        })
        alertController.addAction(UIAlertAction(title: "Decline", style: .default) { _ in
            sipCall.hangup(486, reason: "Busy Here")
        })
        
        self.present(alertController, animated: true, completion: nil)
        
        calls.insert(sipCall)
        callsTableView.reloadData()
    }
    
    func onCallRinging(_ sipCall: SipCall) {
        logsTextView.text += "Call ringing from \(sipCall.remoteUri)...\n";
    }
    
    func onCallProcess(_ sipCall: SipCall) {
        logsTextView.text += "Call process from \(sipCall.remoteUri)...\n";
    }
    
    func onCallEstablished(_ sipCall: SipCall) {
        logsTextView.text += "Call established from \(sipCall.remoteUri)...\n";
    }
    
    func onCallClosed(_ sipCall: SipCall) {
        logsTextView.text += "Call closed from \(sipCall.remoteUri)...\n";
        
        calls.remove(sipCall)
        callsTableView.reloadData()
    }
}

extension SipCallsViewController : UITableViewDataSource {
    func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        return calls.count
    }
    
    func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "CellId", for: indexPath) as! CallCell
        let calls = Array(self.calls)
        let sipCall = calls[indexPath.row]
        cell.remoteUriLabel.text = sipCall.remoteUri
        return cell
    }
}
