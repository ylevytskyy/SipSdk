//
//  CallCell.swift
//  SipSdk
//
//  Created by Yuriy Levytskyy on 2/28/19.
//  Copyright © 2019 Yuriy Levytskyy. All rights reserved.
//

import UIKit

class CallCell: UITableViewCell {
    @IBOutlet weak var remoteUriLabel: UILabel!

    private let badgeLabel: UILabel = {
        let label = UILabel()
        label.font = UIFont.systemFont(ofSize: 11, weight: .semibold)
        label.textColor = .white
        label.backgroundColor = .systemBlue
        label.layer.cornerRadius = 6
        label.layer.masksToBounds = true
        label.textAlignment = .center
        label.translatesAutoresizingMaskIntoConstraints = false
        return label
    }()

    override func awakeFromNib() {
        super.awakeFromNib()
        setupBadgeIfNeeded()
    }

    private func setupBadgeIfNeeded() {
        guard badgeLabel.superview == nil else { return }
        contentView.addSubview(badgeLabel)
        NSLayoutConstraint.activate([
            badgeLabel.trailingAnchor.constraint(equalTo: contentView.trailingAnchor, constant: -12),
            badgeLabel.centerYAnchor.constraint(equalTo: contentView.centerYAnchor),
            badgeLabel.widthAnchor.constraint(greaterThanOrEqualToConstant: 56),
            badgeLabel.heightAnchor.constraint(equalToConstant: 22)
        ])
    }

    func configureWithActiveCall(_ call: SipCall) {
        setupBadgeIfNeeded()
        remoteUriLabel?.text = call.remoteUri
        remoteUriLabel?.font = UIFont.systemFont(ofSize: 15, weight: .medium)
        badgeLabel.text = call.hasVideo ? " VIDEO " : " AUDIO "
        badgeLabel.backgroundColor = call.hasVideo ? .systemGreen : .systemBlue
        accessoryType = .disclosureIndicator
    }

    func configureWithHistoryEntry(_ entry: SipCallHistoryEntry) {
        setupBadgeIfNeeded()
        remoteUriLabel?.text = entry.formattedSummary
        remoteUriLabel?.font = UIFont.systemFont(ofSize: 13, weight: .regular)
        badgeLabel.text = entry.hasVideo ? " VIDEO " : " AUDIO "
        badgeLabel.backgroundColor = .systemGray
        accessoryType = .none
    }
}
